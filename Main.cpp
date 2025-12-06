#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace Utils {

static constexpr size_t roundUp4(size_t N) {
  size_t Mask = ~size_t(3); // 0b...11111100
  return (N + 3) & Mask;
}

} // namespace Utils

namespace Wayland {

class RingBuffer {
public:
  RingBuffer() {
    size_t PageSize = sysconf(_SC_PAGESIZE);
    Capacity = PageSize; // must be multiple of page size

    // allocate underlying memory
    int Fd = memfd_create("ring-buffer", MFD_CLOEXEC);
    if (Fd == -1)
      throw std::system_error(errno, std::generic_category(),
                              "failed to create ring buffer memfd");

    if (ftruncate(Fd, Capacity) == -1) {
      close(Fd);
      throw std::system_error(errno, std::generic_category(),
                              "failed to size ring buffer");
    }

    // reserve virtual address space
    void *Addr = mmap(nullptr, Capacity * 2, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Addr == MAP_FAILED) {
      close(Fd);
      throw std::system_error(errno, std::generic_category(),
                              "failed to reserve ring buffer address space");
    }

    // create magic ring buffer mapping
    void *First = mmap(Data, Capacity, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, Fd, 0);
    void *Second = mmap(Data + Capacity, Capacity, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FIXED, Fd, 0);
    close(Fd); // can close fd after mapping
    if (First == MAP_FAILED || Second == MAP_FAILED) {
      munmap(Data, Capacity * 2);
      throw std::system_error(errno, std::generic_category(),
                              "failed to map ring buffer");
    }

    Data = static_cast<char *>(Addr);
  }

  ~RingBuffer() {
    if (Data)
      munmap(Data, Capacity * 2);
  }

  // prohibit copy/move
  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;
  RingBuffer(RingBuffer &&) = delete;
  RingBuffer &operator=(RingBuffer &&) = delete;

  char *writePtr() { return Data + (Head % Capacity); }
  size_t writeAvailable() const { return Capacity - (Head - Tail); }
  void advanceRead(size_t N) { Head += N; }

  const char *readPtr() const { return Data + (Tail % Capacity); }
  size_t readAvailable() const { return Head - Tail; }
  void advanceWrite(size_t N) { Tail += N; }

  size_t capacity() const { return Capacity; }
  bool full() const { return Head - Tail >= Capacity; }

private:
  char *Data{nullptr};
  size_t Capacity{0};
  size_t Head{0};
  size_t Tail{0};
};

class MessageView {
public:
  MessageView(const char *Data, size_t Size) : Data{Data}, Size{Size}, Pos{0} {}

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  T readUint() {
    assert(Pos + sizeof(T) <= Size);
    assert(Pos % alignof(T) == 0);

    T Value;
    std::memcpy(&Value, Data + Pos, sizeof(T));
    Pos += sizeof(T);
    return Value;
  }

  std::string readString() {
    uint32_t Len = readUint<uint32_t>();
    assert(Pos + Len <= Size);
    assert(Utils::roundUp4(Len) == Len); // TODO: this may be too strict

    std::string Result{Data + Pos, Len};
    Pos += Len;
    return Result;
  }

  const char *data() const { return Data; }
  size_t size() const { return Size; }
  size_t pos() const { return Pos; }

private:
  const char *Data;
  size_t Size;
  size_t Pos;
};

class MessageDraft {
public:
  MessageDraft() : Data{}, Pos{0} {}

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  void writeUint(T Value) {
    assert(Pos % alignof(T) == 0);

    size_t OldSize = Data.size();
    Data.resize(OldSize + sizeof(T));
    std::memcpy(Data.data() + OldSize, &Value, sizeof(T));
    Pos += sizeof(T);
  }

  void writeString(std::string_view Str) {
    // write length first as per Wayland protocol
    writeUint(static_cast<uint32_t>(Str.size()));

    size_t PaddedLen = Utils::roundUp4(Str.size());
    size_t OldSize = Data.size();
    Data.resize(OldSize + PaddedLen, '\0');
    std::memcpy(Data.data() + Pos, Str.data(), Str.size());
    Pos += PaddedLen;
  }

  const char *data() const { return Data.data(); }
  size_t size() const { return Data.size(); }
  size_t pos() const { return Pos; }

private:
  std::vector<char> Data;
  size_t Pos;
};

class SharedMemory {
public:
  SharedMemory(uint32_t Width, uint32_t Height, uint32_t Channels);
  ~SharedMemory();

  SharedMemory(const SharedMemory &) = delete;
  SharedMemory &operator=(const SharedMemory &) = delete;
  SharedMemory(SharedMemory &&) = delete;
  SharedMemory &operator=(SharedMemory &&) = delete;

  int fd() const { return Fd; }
  uint32_t stride() const { return Stride; }
  uint32_t width() const { return Width; }
  uint32_t height() const { return Height; }
  uint32_t poolSize() const { return PoolSize; }
  uint8_t *poolData() const { return PoolData; }

private:
  int Fd{-1};
  uint32_t Stride{};
  uint32_t Width{};
  uint32_t Height{};
  uint32_t PoolSize{};
  uint8_t *PoolData{nullptr};
};

SharedMemory::SharedMemory(uint32_t Width, uint32_t Height, uint32_t Channels)
    : Width{Width}, Height{Height} {
  Stride = Width * Channels;
  PoolSize = Stride * Height;

  // create anonymous shared memory file in memory
  Fd = memfd_create("wayland-shm", MFD_CLOEXEC);
  if (Fd == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to create shared memory");

  // set size
  if (ftruncate(Fd, PoolSize) == -1) {
    int SavedErrno = errno;
    close(Fd);
    Fd = -1;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to truncate shared memory");
  }

  // map memory
  // PROT_READ | PROT_WRITE for read/write
  // MAP_SHARED for sharing with compositor process
  PoolData = static_cast<uint8_t *>(
      mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0));
  if (PoolData == MAP_FAILED) {
    int SavedErrno = errno;
    close(Fd);
    Fd = -1;
    PoolData = nullptr;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to map shared memory");
  }
}

SharedMemory::~SharedMemory() {
  if (PoolData)
    munmap(PoolData, PoolSize);
  if (Fd != -1)
    close(Fd);
}

static constexpr uint32_t HeaderSize =
    8; // object id (4) + opcode (2) + size (2)
static constexpr uint32_t ColorChannels = 4; // rgba

static constexpr uint32_t DisplayObjectId = 1; // singleton display object

namespace Event {
static constexpr uint16_t WlRegistryGlobal = 0;
static constexpr uint16_t ShmPoolFormat = 0;
static constexpr uint16_t WlBufferRelease = 0;
static constexpr uint16_t XdgWmBasePing = 0;
static constexpr uint16_t XdgToplevelConfigure = 0;
static constexpr uint16_t XdgToplevelClose = 1;
static constexpr uint16_t XdgSurfaceConfigure = 0;
static constexpr uint16_t WlDisplayError = 0;
} // namespace Event

namespace Opcode {
static constexpr uint16_t WlDisplayGetRegistry = 1;
static constexpr uint16_t WlRegistryBind = 0;
static constexpr uint16_t WlCompositorCreateSurface = 0;
static constexpr uint16_t XdgWmBasePong = 3;
static constexpr uint16_t XdgSurfaceAckConfigure = 4;
static constexpr uint16_t WlShmCreatePool = 0;
static constexpr uint16_t XdgWmBaseGetXdgSurface = 2;
static constexpr uint16_t WlShmPoolCreateBuffer = 0;
static constexpr uint16_t WlSurfaceAttach = 1;
static constexpr uint16_t XdgSurfaceGetToplevel = 1;
static constexpr uint16_t WlSurfaceCommit = 6;
} // namespace Opcode

namespace Format {
static constexpr uint32_t Xrgb8888 = 1;
} // namespace Format

class Display {
public:
  enum class Status {
    None,
    SurfaceAckedConfigure,
    SurfaceAttached,
  };

  struct State {
    uint32_t WlRegistry{};
    uint32_t WlShm{};
    uint32_t WlShmPool{};
    uint32_t WlBuffer{};
    uint32_t WlCompositor{};
    uint32_t WlSurface{};
    uint32_t XdgToplevel{};
    uint32_t XdgWmBase{};
    uint32_t XdgSurface{};
  };

  Display();
  ~Display();

  // prohibit copy/move
  Display(const Display &) = delete;
  Display &operator=(const Display &) = delete;
  Display(Display &&) = delete;
  Display &operator=(Display &&) = delete;

  void run();

  // Wayland protocol methods
  uint32_t wlGetRegistry();
  uint32_t wlRegistryBind(uint32_t Name, std::string_view Interface,
                          uint32_t Version);
  uint32_t wlCompositorCreateSurface();
  uint32_t wlShmCreatePool();
  uint32_t wlShmPoolCreateBuffer();
  void wlSurfaceAttach();
  void wlSurfaceCommit();
  uint32_t xdgWmBaseGetXdgSurface();
  uint32_t xdgSurfaceGetToplevel();
  void xdgWmBasePong(uint32_t Ping);
  void xdgSurfaceAckConfigure(uint32_t Configure);

  void handleMessage(MessageView &Msg);

private:
  void handleRegistryGlobal(MessageView &Msg);
  void handleDisplayError(MessageView &Msg);

  RingBuffer RecvBuffer;

  int Fd = -1;
  std::string RuntimeDir;
  std::string DisplayName;

  State State;
  Status Status{Status::None};
  SharedMemory SharedMemory{117, 150, ColorChannels};
  uint32_t CurrentObjectId = 1;
};

Display::Display() {
  // get env vars
  char *XdgRuntimeDirEnv = std::getenv("XDG_RUNTIME_DIR");
  if (!XdgRuntimeDirEnv)
    throw std::runtime_error("XDG_RUNTIME_DIR not set");
  RuntimeDir = std::string(XdgRuntimeDirEnv);

  char *WaylandDisplayEnv = std::getenv("WAYLAND_DISPLAY");
  DisplayName =
      WaylandDisplayEnv ? std::string(WaylandDisplayEnv) : "wayland-0";

  // prepare socket path
  std::string SocketPath = RuntimeDir + "/" + DisplayName;
  if (SocketPath.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("socket path too long");

  // prepare Unix socket address
  struct sockaddr_un Addr = {.sun_family = AF_UNIX};
  SocketPath.copy(Addr.sun_path, SocketPath.size());
  Addr.sun_path[SocketPath.size()] = '\0';

  // create Unix socket
  Fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (Fd == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to create socket");

  // connect to socket
  struct sockaddr *SockAddr = reinterpret_cast<struct sockaddr *>(&Addr);
  if (::connect(Fd, SockAddr, sizeof(SockAddr)) == -1) {
    int SavedErrno = errno;
    close(Fd);
    Fd = -1;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to connect to wayland display");
  }

  // get wayland registry
  State.WlRegistry = wlGetRegistry();
}

Display::~Display() {
  if (Fd != -1)
    close(Fd);
}

void Display::run() {
  while (true) {
    if (RecvBuffer.full())
      throw std::runtime_error("ring buffer full");

    ssize_t RecvLen =
        recv(Fd, RecvBuffer.writePtr(), RecvBuffer.writeAvailable(), 0);
    if (RecvLen == -1) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to receive message");
    }
    if (RecvLen == 0) {
      throw std::runtime_error("wayland display connection closed");
    }

    RecvBuffer.advanceRead(RecvLen);

    while (RecvBuffer.readAvailable() >= HeaderSize) {
      uint16_t MsgSize;
      std::memcpy(&MsgSize, RecvBuffer.readPtr() + 6, sizeof(MsgSize));

      if (RecvBuffer.readAvailable() < MsgSize)
        break;

      MessageView Msg(RecvBuffer.readPtr(), MsgSize);
      RecvBuffer.advanceWrite(MsgSize);

      handleMessage(Msg);
    }
  }
}

// uint32_t wl_get_registry(uint32_t new_id)
uint32_t Display::wlGetRegistry() {
  MessageDraft Msg;
  Msg.writeUint(DisplayObjectId);
  Msg.writeUint(Opcode::WlDisplayGetRegistry);

  // message size (part of header)
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentObjectId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // argument to get_registry
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), MSG_DONTWAIT);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send get_registry message");

  return CurrentObjectId;
}

// uint32_t wl_registry_bind(uint32_t name, const char *interface, uint32_t
// version)
uint32_t Display::wlRegistryBind(uint32_t Name, std::string_view Interface,
                                 uint32_t Version) {
  MessageDraft Msg;
  Msg.writeUint(State.WlRegistry);
  Msg.writeUint(Opcode::WlRegistryBind);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(Name) +
                              Utils::roundUp4(Interface.size()) +
                              sizeof(Version) + sizeof(CurrentObjectId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write bind arguments
  Msg.writeUint(Name);
  Msg.writeString(Interface);
  Msg.writeUint(Version);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // verify buffer size is aligned
  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send registry bind message");

  return CurrentObjectId;
}

// uint32_t wl_compositor_create_surface()
uint32_t Display::wlCompositorCreateSurface() {
  assert(State.WlCompositor > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlCompositor);
  Msg.writeUint(Opcode::WlCompositorCreateSurface);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentObjectId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send compositor create_surface message");

  return CurrentObjectId;
}

// uint32_t wl_shm_create_pool(int fd, uint32_t size)
uint32_t Display::wlShmCreatePool() {
  assert(SharedMemory.poolSize() > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlShm);
  Msg.writeUint(Opcode::WlShmCreatePool);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentObjectId) + sizeof(uint32_t);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // write pool size
  Msg.writeUint(SharedMemory.poolSize());

  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send the file descriptor as ancillary data
  int ShmFd = SharedMemory.fd();
  char Buf[CMSG_SPACE(sizeof(ShmFd))]{};

  struct iovec Io = {.iov_base = const_cast<char *>(Msg.data()),
                     .iov_len = Msg.size()};
  struct msghdr SocketMsg = {
      .msg_name = nullptr,
      .msg_namelen = 0,
      .msg_iov = &Io,
      .msg_iovlen = 1,
      .msg_control = Buf,
      .msg_controllen = sizeof(Buf),
  };

  struct cmsghdr *Cmsg = CMSG_FIRSTHDR(&SocketMsg);
  Cmsg->cmsg_level = SOL_SOCKET;
  Cmsg->cmsg_type = SCM_RIGHTS;
  Cmsg->cmsg_len = CMSG_LEN(sizeof(ShmFd));

  *reinterpret_cast<int *>(CMSG_DATA(Cmsg)) = ShmFd;
  SocketMsg.msg_controllen = CMSG_SPACE(sizeof(ShmFd));

  if (sendmsg(Fd, &SocketMsg, 0) == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_shm create_pool message");

  return CurrentObjectId;
}

// uint32_t shm_pool_create_buffer(uint32_t offset, uint32_t width, uint32_t
// height, uint32_t stride, uint32_t format)
uint32_t Display::wlShmPoolCreateBuffer() {
  assert(State.WlShmPool > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlShmPool);
  Msg.writeUint(Opcode::WlShmPoolCreateBuffer);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentObjectId) + sizeof(uint32_t) * 5;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // write buffer parameters
  uint32_t Offset = 0;
  Msg.writeUint(Offset);
  Msg.writeUint(SharedMemory.width());
  Msg.writeUint(SharedMemory.height());
  Msg.writeUint(SharedMemory.stride());
  Msg.writeUint(Format::Xrgb8888);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_shm_pool create_buffer message");

  return CurrentObjectId;
}

// void wl_surface_attach(wl_buffer *buffer, int32_t x, int32_t y)
void Display::wlSurfaceAttach() {
  assert(State.WlSurface > 0);
  assert(State.WlBuffer > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlSurface);
  Msg.writeUint(Opcode::WlSurfaceAttach);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(State.WlBuffer) + sizeof(uint32_t) * 2;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write buffer and position
  Msg.writeUint(State.WlBuffer);
  uint32_t X = 0, Y = 0;
  Msg.writeUint(X);
  Msg.writeUint(Y);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_surface attach message");
}

// void surface_commit()
void Display::wlSurfaceCommit() {
  assert(State.WlSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlSurface);
  Msg.writeUint(Opcode::WlSurfaceCommit);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_surface commit message");
}

// uint32_t xdg_wm_base_get_xdg_surface(uint32_t new_id)
uint32_t Display::xdgWmBaseGetXdgSurface() {
  assert(State.XdgWmBase > 0);
  assert(State.WlSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.XdgWmBase);
  Msg.writeUint(Opcode::XdgWmBaseGetXdgSurface);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentObjectId) + sizeof(State.WlSurface);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // write wl_surface argument
  Msg.writeUint(State.WlSurface);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(
        errno, std::generic_category(),
        "failed to send xdg_wm_base get_xdg_surface message");

  return CurrentObjectId;
}

// uint32_t xdg_surface_get_toplevel(uint32_t new_id)
uint32_t Display::xdgSurfaceGetToplevel() {
  assert(State.XdgSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.XdgSurface);
  Msg.writeUint(Opcode::XdgSurfaceGetToplevel);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentObjectId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentObjectId++;
  Msg.writeUint(CurrentObjectId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send xdg_surface get_toplevel message");

  return CurrentObjectId;
}

// void xdg_wm_base_pong(uint32_t ping)
void Display::xdgWmBasePong(uint32_t Ping) {
  assert(State.XdgWmBase > 0);
  assert(State.WlSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.XdgWmBase);
  Msg.writeUint(Opcode::XdgWmBasePong);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(Ping);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write ping argument
  Msg.writeUint(Ping);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send xdg_wm_base pong message");
}

// void xdg_surface_ack_configure(uint32_t configure)
void Display::xdgSurfaceAckConfigure(uint32_t Configure) {
  assert(State.XdgSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.XdgSurface);
  Msg.writeUint(Opcode::XdgSurfaceAckConfigure);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(Configure);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write configure argument
  Msg.writeUint(Configure);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send xdg_surface ack_configure message");
}

void Display::handleMessage(MessageView &Msg) {
  assert(Msg.size() >= 8);

  // read header
  uint32_t ObjectId = Msg.readUint<uint32_t>();
  assert(ObjectId <= CurrentObjectId);

  uint16_t Opcode = Msg.readUint<uint16_t>();
  uint16_t MsgSizeAnnounced = Msg.readUint<uint16_t>();
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  assert(MsgSizeAnnounced <= Msg.size());

  // dispatch based on object and opcode
  if (ObjectId == State.WlRegistry && Opcode == Event::WlRegistryGlobal) {
    handleRegistryGlobal(Msg);
  } else if (ObjectId == DisplayObjectId && Opcode == Event::WlDisplayError) {
    handleDisplayError(Msg);
  } else {
    // TODO: add more event handlers
  }
}

void Display::handleRegistryGlobal(MessageView &Msg) {
  uint32_t Name = Msg.readUint<uint32_t>();
  std::string Interface = Msg.readString();
  uint32_t Version = Msg.readUint<uint32_t>();

  // bind interfaces we need
  if (Interface == "wl_shm") {
    State.WlShm = wlRegistryBind(Name, Interface, Version);
  } else if (Interface == "xdg_wm_base") {
    State.XdgWmBase = wlRegistryBind(Name, Interface, Version);
  } else if (Interface == "wl_compositor") {
    State.WlCompositor = wlRegistryBind(Name, Interface, Version);
  }
}

void Display::handleDisplayError(MessageView &Msg) {
  uint32_t TargetObjectId = Msg.readUint<uint32_t>();
  uint32_t Code = Msg.readUint<uint32_t>();
  std::string Error = Msg.readString();

  std::ostringstream Stream;
  Stream << "fatal wayland error: target_object_id=" << TargetObjectId
         << " code=" << Code << " error=" << Error;
  throw std::runtime_error(Stream.str());
}
} // namespace Wayland

int main() {
  try {
    Wayland::Display Display;
    Display.run();
  } catch (const std::exception &Exception) {
    std::cerr << "error: " << Exception.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}