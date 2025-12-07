#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "../external/stb_image.h"

namespace Utils {

static constexpr size_t roundUp4(size_t N) {
  size_t Mask = ~size_t(3); // 0b...11111100
  return (N + 3) & Mask;
}

} // namespace Utils

class Animation {
public:
  explicit Animation(const std::string &FrameDir, size_t FrameCount);

  const std::vector<uint8_t> &frame(size_t Index) const {
    assert(Index < Frames.size());
    return Frames[Index];
  }
  size_t frameCount() const { return Frames.size(); }
  size_t width() const { return Width; }
  size_t height() const { return Height; }
  size_t channels() const { return Channels; }

private:
  std::vector<std::vector<uint8_t>> Frames;
  size_t Width{};
  size_t Height{};
  size_t Channels{};
};

Animation::Animation(const std::string &FrameDir, size_t FrameCount) {
  Frames.reserve(FrameCount);

  for (size_t I = 1; I <= FrameCount; ++I) {
    std::ostringstream Path;
    Path << FrameDir << "/frame_";
    Path << std::setfill('0') << std::setw(4) << I;
    Path << ".png";

    int W, H, C;
    uint8_t *Data = stbi_load(Path.str().c_str(), &W, &H, &C, 4);
    if (!Data)
      throw std::runtime_error("failed to load frame: " + Path.str());

    Width = static_cast<size_t>(W);
    Height = static_cast<size_t>(H);
    Channels = static_cast<size_t>(C);

    size_t DataSize = Width * Height * 4;
    std::vector<uint8_t> Pixels{Data, Data + DataSize};
    Frames.push_back(std::move(Pixels));
    stbi_image_free(Data);
  }
}

namespace Wayland {

class RingBuffer {
public:
  explicit RingBuffer();
  /* virtual */ ~RingBuffer();

  // prohibit copy/move
  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;
  RingBuffer(RingBuffer &&) = delete;
  RingBuffer &operator=(RingBuffer &&) = delete;

  char *writePtr() { return Data + (Head % Capacity); }
  const char *readPtr() const { return Data + (Tail % Capacity); }

  size_t sizeWritable() const { return Capacity - (Head - Tail); }
  size_t sizeReadable() const { return Head - Tail; }

  void advanceWrite(size_t N) { Head += N; }
  void advanceRead(size_t N) { Tail += N; }

  size_t capacity() const { return Capacity; }
  bool full() const { return Head - Tail >= Capacity; }

private:
  char *Data{nullptr};
  size_t Capacity{0};

  // [......Tail======Head......]
  size_t Head{0}; // next write position
  size_t Tail{0}; // next read position
};

RingBuffer::RingBuffer() {
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
  Data = static_cast<char *>(mmap(nullptr, Capacity * 2, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Data == MAP_FAILED) {
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
}

RingBuffer::~RingBuffer() {
  if (Data)
    munmap(Data, Capacity * 2);
}

class MessageView {
public:
  explicit MessageView(const char *Data, size_t Size)
      : Data{Data}, Size{Size}, Pos{0} {}

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
    uint32_t PaddedLen = Utils::roundUp4(Len);
    assert(Pos + PaddedLen <= Size);

    std::string Result{Data + Pos, Len - 1}; // exclude null terminator
    Pos += PaddedLen;
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
  explicit MessageDraft() = default;

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  void writeUint(T Value) {
    assert(Pos % alignof(T) == 0);

    size_t OldSize = Data.size();
    Data.resize(OldSize + sizeof(T));
    std::memcpy(Data.data() + OldSize, &Value, sizeof(T));
    Pos += sizeof(T);
  }

  void writeString(std::string_view Str) {
    uint32_t Len = Str.size() + 1; // include null terminator
    writeUint(Len);

    size_t PaddedLen = Utils::roundUp4(Len);
    Data.resize(Data.size() + PaddedLen, '\0'); // zeros include null + padding
    std::memcpy(Data.data() + Pos, Str.data(), Str.size());
    Pos += PaddedLen;
  }

  const char *data() const { return Data.data(); }
  size_t size() const { return Data.size(); }
  size_t pos() const { return Pos; }

private:
  std::vector<char> Data{};
  size_t Pos{0};
};

class SharedMemory {
public:
  explicit SharedMemory(size_t Width, size_t Height, size_t Channels);
  /* virtual */ ~SharedMemory();

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

SharedMemory::SharedMemory(size_t Width, size_t Height, size_t Channels)
    : Width{static_cast<uint32_t>(Width)},
      Height{static_cast<uint32_t>(Height)} {
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
static constexpr uint16_t WlSurfaceDamage = 2;
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

  explicit Display();
  /* virtual */ ~Display();

  // prohibit copy/move
  Display(const Display &) = delete;
  Display &operator=(const Display &) = delete;
  Display(Display &&) = delete;
  Display &operator=(Display &&) = delete;

  void run();

private:
  static constexpr uint32_t HeaderSize = 8;
  static constexpr uint32_t MsgSizeOffset = 6;
  static constexpr uint32_t DisplayObjectId = 1;
  static constexpr int FrameDelayMs = 100; // ~10 fps

  uint32_t wlGetRegistry();
  uint32_t wlRegistryBind(uint32_t Name, std::string_view Interface,
                          uint32_t Version);
  uint32_t wlCompositorCreateSurface();
  uint32_t wlShmCreatePool();
  uint32_t wlShmPoolCreateBuffer();
  void wlSurfaceAttach();
  void wlSurfaceDamage();
  void wlSurfaceCommit();
  uint32_t xdgWmBaseGetXdgSurface();
  uint32_t xdgSurfaceGetToplevel();
  void xdgWmBasePong(uint32_t Ping);
  void xdgSurfaceAckConfigure(uint32_t Configure);

  void handleMessage(MessageView &Msg);
  void handleRegistryGlobal(MessageView &Msg);
  void handleDisplayError(MessageView &Msg);
  void handleShmFormat(MessageView &Msg);
  void handleBufferRelease(MessageView &Msg);
  void handleXdgWmBasePing(MessageView &Msg);
  void handleXdgToplevelConfigure(MessageView &Msg);
  void handleXdgToplevelClose(MessageView &Msg);
  void handleXdgSurfaceConfigure(MessageView &Msg);
  void handleStatusNone();
  void handleStatusSurfaceAckedConfigure();
  void handleStatusSurfaceAttached();

  bool pollEvents(int TimeoutMs);
  void renderFrame();

  int Fd = -1;
  std::string RuntimeDir{};
  std::string DisplayName{};

  State State{};
  Status Status{Status::None};
  RingBuffer Buffer{};
  Animation Anim{"src/frames", 311};
  SharedMemory Pixels{Anim.width(), Anim.height(), Anim.channels()};
  uint32_t CurrentObjectId = 1;
  size_t CurrentFrameIdx = 0;
};

Display::Display() {
  // get env vars
  char *XdgRuntimeDirEnv = std::getenv("XDG_RUNTIME_DIR");
  if (!XdgRuntimeDirEnv)
    throw std::runtime_error("XDG_RUNTIME_DIR not set");
  RuntimeDir = std::string{XdgRuntimeDirEnv};

  char *WaylandDisplayEnv = std::getenv("WAYLAND_DISPLAY");
  DisplayName =
      WaylandDisplayEnv ? std::string{WaylandDisplayEnv} : "wayland-0";

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
  if (connect(Fd, SockAddr, sizeof(Addr)) == -1) {
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
    switch (Status) {
    case Status::None:
      handleStatusNone();
      break;
    case Status::SurfaceAckedConfigure:
      handleStatusSurfaceAckedConfigure();
      break;
    case Status::SurfaceAttached:
      handleStatusSurfaceAttached();
      break;
    }

    int Timeout = (Status == Status::SurfaceAttached) ? FrameDelayMs : -1;
    bool HasNewMessages = pollEvents(Timeout);
    if (!HasNewMessages)
      continue;

    ssize_t RecvLen = recv(Fd, Buffer.writePtr(), Buffer.sizeWritable(), 0);
    if (RecvLen == -1)
      throw std::system_error(errno, std::generic_category(),
                              "failed to receive message");
    if (RecvLen == 0)
      throw std::runtime_error("wayland display connection closed");

    Buffer.advanceWrite(RecvLen);

    while (Buffer.sizeReadable() >= HeaderSize) {
      uint16_t MsgSize;
      std::memcpy(&MsgSize, Buffer.readPtr() + MsgSizeOffset, sizeof(MsgSize));
      if (Buffer.sizeReadable() < MsgSize)
        break;

      MessageView Msg{Buffer.readPtr(), MsgSize};
      Buffer.advanceRead(MsgSize);
      handleMessage(Msg);
    }
  }
}

bool Display::pollEvents(int TimeoutMs) {
  if (Buffer.full())
    throw std::runtime_error("ring buffer full");

  struct pollfd Pfd = {.fd = Fd, .events = POLLIN, .revents = 0};
  int Ret = poll(&Pfd, 1, TimeoutMs);

  if (Ret == -1)
    throw std::system_error(errno, std::generic_category(), "poll failed");
  if (Ret == 0)
    return false; // timeout
  return true;
}

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
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
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

  // calculate message size (+1 for null terminator, +4 for string length
  // prefix)
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(Name) + sizeof(uint32_t) +
                              Utils::roundUp4(Interface.size() + 1) +
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
  assert(Pixels.poolSize() > 0);

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
  Msg.writeUint(Pixels.poolSize());

  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send the file descriptor as ancillary data
  // so that inter-process conversion can happen automatically
  int ShmFd = Pixels.fd();
  char ShmFdBuf[CMSG_SPACE(sizeof(ShmFd))]{};

  struct iovec Io = {.iov_base = const_cast<char *>(Msg.data()),
                     .iov_len = Msg.size()};
  struct msghdr SocketMsg = {
      .msg_name = nullptr,
      .msg_namelen = 0,
      .msg_iov = &Io, // regular data (message)
      .msg_iovlen = 1,
      .msg_control = ShmFdBuf, // ancillary data (file descriptor)
      .msg_controllen = sizeof(ShmFdBuf),
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
  Msg.writeUint(Pixels.width());
  Msg.writeUint(Pixels.height());
  Msg.writeUint(Pixels.stride());
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

// void wl_surface_damage(int32_t x, int32_t y, int32_t width, int32_t height)
void Display::wlSurfaceDamage() {
  assert(State.WlSurface > 0);

  MessageDraft Msg;
  Msg.writeUint(State.WlSurface);
  Msg.writeUint(Opcode::WlSurfaceDamage);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(int32_t) * 4;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // damage entire surface
  Msg.writeUint<int32_t>(0);
  Msg.writeUint<int32_t>(0);
  Msg.writeUint<int32_t>(Pixels.width());
  Msg.writeUint<int32_t>(Pixels.height());

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_surface damage message");
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
  } else if (ObjectId == State.WlShm && Opcode == Event::ShmPoolFormat) {
    handleShmFormat(Msg);
  } else if (ObjectId == State.WlBuffer && Opcode == Event::WlBufferRelease) {
    handleBufferRelease(Msg);
  } else if (ObjectId == State.XdgWmBase && Opcode == Event::XdgWmBasePing) {
    handleXdgWmBasePing(Msg);
  } else if (ObjectId == State.XdgToplevel &&
             Opcode == Event::XdgToplevelConfigure) {
    handleXdgToplevelConfigure(Msg);
  } else if (ObjectId == State.XdgToplevel &&
             Opcode == Event::XdgToplevelClose) {
    handleXdgToplevelClose(Msg);
  } else if (ObjectId == State.XdgSurface &&
             Opcode == Event::XdgSurfaceConfigure) {
    handleXdgSurfaceConfigure(Msg);
  } else {
    std::cerr << "unhandled event: object_id=" << ObjectId
              << " opcode=" << Opcode << std::endl;
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

void Display::handleShmFormat(MessageView &Msg) {
  uint32_t Format = Msg.readUint<uint32_t>();
  std::cout << "<- wl_shm: format=" << std::hex << Format << std::dec
            << std::endl;
}

void Display::handleBufferRelease(MessageView &) {
  // skip release for now
  std::cout << "<- wl_buffer@" << State.WlBuffer << ".release" << std::endl;
}

void Display::handleXdgWmBasePing(MessageView &Msg) {
  uint32_t Ping = Msg.readUint<uint32_t>();
  std::cout << "<- xdg_wm_base@" << State.XdgWmBase << ".ping: ping=" << Ping
            << std::endl;
  xdgWmBasePong(Ping);
}

void Display::handleXdgToplevelConfigure(MessageView &Msg) {
  uint32_t Width = Msg.readUint<uint32_t>();
  uint32_t Height = Msg.readUint<uint32_t>();
  uint32_t StatesLen = Msg.readUint<uint32_t>();
  // skip states array
  for (uint32_t I = 0; I < StatesLen / sizeof(uint32_t); ++I)
    Msg.readUint<uint32_t>();

  std::cout << "<- xdg_toplevel@" << State.XdgToplevel
            << ".configure: w=" << Width << " h=" << Height << " states["
            << StatesLen << "]" << std::endl;
}

void Display::handleXdgToplevelClose(MessageView &) {
  std::cout << "<- xdg_toplevel@" << State.XdgToplevel << ".close" << std::endl;
  std::exit(EXIT_SUCCESS);
}

void Display::handleXdgSurfaceConfigure(MessageView &Msg) {
  uint32_t Configure = Msg.readUint<uint32_t>();
  std::cout << "<- xdg_surface@" << State.XdgSurface
            << ".configure: configure=" << Configure << std::endl;
  xdgSurfaceAckConfigure(Configure);
  Status = Status::SurfaceAckedConfigure;
}

void Display::handleStatusNone() {
  assert(Status == Status::None);

  if (State.WlCompositor == 0 || State.WlShm == 0 || State.XdgWmBase == 0)
    return; // bind is not complete yet
  if (State.WlSurface != 0)
    return; // already created surface

  State.WlSurface = wlCompositorCreateSurface();
  State.XdgSurface = xdgWmBaseGetXdgSurface(); // not used yet
  State.XdgToplevel = xdgSurfaceGetToplevel(); // not used yet
  wlSurfaceCommit();
}

void Display::handleStatusSurfaceAckedConfigure() {
  assert(Status == Status::SurfaceAckedConfigure);

  assert(State.WlSurface != 0);
  assert(State.XdgSurface != 0);
  assert(State.XdgToplevel != 0);

  if (State.WlShmPool == 0)
    State.WlShmPool = wlShmCreatePool();
  if (State.WlBuffer == 0)
    State.WlBuffer = wlShmPoolCreateBuffer();

  renderFrame();
  wlSurfaceAttach();
  wlSurfaceDamage();
  wlSurfaceCommit();
  Status = Status::SurfaceAttached;
}

void Display::handleStatusSurfaceAttached() {
  assert(Status == Status::SurfaceAttached);

  renderFrame();
  wlSurfaceAttach(); // maybe unnecessary
  wlSurfaceDamage();
  wlSurfaceCommit();
}

void Display::renderFrame() {
  assert(Pixels.poolData() != nullptr);
  assert(Pixels.poolSize() != 0);

  const auto &Frame = Anim.frame(CurrentFrameIdx);
  uint32_t *PixelData = reinterpret_cast<uint32_t *>(Pixels.poolData());

  // convert RGBA to XRGB8888
  size_t PixelCount = Anim.width() * Anim.height();
  for (size_t I = 0; I < PixelCount; ++I) {
    uint8_t R = Frame[I * 4 + 0];
    uint8_t G = Frame[I * 4 + 1];
    uint8_t B = Frame[I * 4 + 2];
    PixelData[I] = (R << 16) | (G << 8) | B;
  }

  CurrentFrameIdx = (CurrentFrameIdx + 1) % Anim.frameCount();
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