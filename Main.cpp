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

class Buffer {
public:
  explicit Buffer() : Data{}, Pos{0} {}
  explicit Buffer(const std::vector<char> &Data) : Data{Data}, Pos{0} {}

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  void writeUint(T Value) {
    assert(Pos % alignof(T) == 0);

    size_t OldSize = Data.size();
    Data.resize(OldSize + sizeof(T));
    std::memcpy(Data.data() + OldSize, &Value, sizeof(T));
    Pos += sizeof(T);
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  T readUint() {
    assert(Pos + sizeof(T) <= Data.size());
    assert(Pos % alignof(T) == 0);

    T Value;
    std::memcpy(&Value, Data.data() + Pos, sizeof(T));
    Pos += sizeof(T);
    return Value;
  }

  void writeString(std::string_view Str) {
    // write length first as per Wayland protocol
    writeUint(static_cast<uint32_t>(Str.size()));

    size_t PaddedLen = roundUp4(Str.size());
    size_t OldSize = Data.size();
    Data.resize(OldSize + PaddedLen, '\0');
    std::memcpy(Data.data() + Pos, Str.data(), Str.size());
    Pos += PaddedLen;
  }

  std::string readString(uint32_t Len) {
    assert(Pos + Len <= Data.size());

    std::string Result{Data.data() + Pos, Len};
    Pos += roundUp4(Len);
    return Result;
  }

  const char *data() const { return Data.data(); }
  size_t size() const { return Data.size(); }
  size_t pos() const { return Pos; }

private:
  std::vector<char> Data;
  size_t Pos;
};

} // namespace Utils

namespace Wayland {

static constexpr uint32_t HeaderSize =
    8; // object id (4) + opcode (2) + size (2)
static constexpr uint32_t ColorChannels = 4; // rgba

static constexpr uint32_t DisplayObjectId = 1; // singleton display object
static constexpr uint16_t WlRegistryEventGlobal = 0;
static constexpr uint16_t ShmPoolEventFormat = 0;
static constexpr uint16_t WlBufferEventRelease = 0;
static constexpr uint16_t XdgWmBaseEventPing = 0;
static constexpr uint16_t XdgToplevelEventConfigure = 0;
static constexpr uint16_t XdgToplevelEventClose = 1;
static constexpr uint16_t XdgSurfaceEventConfigure = 0;
static constexpr uint16_t WlDisplayErrorEvent = 0;

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
    uint32_t XdgWmBase{};
    uint32_t XdgSurface{};
    uint32_t WlCompositor{};
    uint32_t WlSurface{};
    uint32_t XdgToplevel{};
  };

  struct SharedMemory {
    int Fd{-1};
    uint32_t Stride{};
    uint32_t Width{};
    uint32_t Height{};
    uint32_t PoolSize{};
    uint8_t *PoolData{nullptr};

    void allocate(size_t Size);
  };

  // TODO: add constructor/destructor
  // TODO: prohibit copy/move
  void initialize();
  void connect();

  // Wayland protocol methods
  uint32_t wlGetRegistry();
  uint32_t wlRegistryBind(uint32_t Name, std::string_view Interface,
                          uint32_t InterfaceLen, uint32_t Version);
  uint32_t wlCompositorCreateSurface();
  uint32_t wlShmCreatePool();
  uint32_t wlXdgWmBaseGetXdgSurface();
  uint32_t wlShmPoolCreateBuffer();
  void wlSurfaceAttach();
  void wlSurfaceCommit();
  uint32_t wlXdgSurfaceGetToplevel();
  void wlXdgWmBasePong(uint32_t Ping);
  void wlXdgSurfaceAckConfigure(uint32_t Configure);

  void handleMessage(Utils::Buffer &Msg);

private:
  void handleRegistryGlobal(Utils::Buffer &Msg, uint16_t MsgSizeAnnounced);
  void handleDisplayError(Utils::Buffer &Msg);

  int Fd = -1;
  uint32_t CurrentId = 1;
  std::string XdgRuntimeDir;
  std::string DisplayName;

  State State;
  Status Status{Status::None};
  SharedMemory SharedMemory;
};

void Display::initialize() {
  // set shared memory parameters
  SharedMemory.Width = 800;
  SharedMemory.Height = 600;
  SharedMemory.Stride = SharedMemory.Width * ColorChannels;
  size_t PoolSize = SharedMemory.Stride * SharedMemory.Height;

  // allocate shared memory
  SharedMemory.allocate(PoolSize);
}

void Display::connect() {
  // get env vars
  char *XdgRuntimeDirEnv = std::getenv("XDG_RUNTIME_DIR");
  if (!XdgRuntimeDirEnv)
    throw std::runtime_error("XDG_RUNTIME_DIR not set");
  XdgRuntimeDir = std::string(XdgRuntimeDirEnv);

  char *WaylandDisplayEnv = std::getenv("WAYLAND_DISPLAY");
  DisplayName =
      WaylandDisplayEnv ? std::string(WaylandDisplayEnv) : "wayland-0";

  // prepare socket path
  std::string SocketPath = XdgRuntimeDir + "/" + DisplayName;
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
}

void Display::SharedMemory::allocate(size_t Size) {
  // create anonymous shared memory file in memory
  Fd = memfd_create("wayland-shm", MFD_CLOEXEC);
  if (Fd == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to create shared memory");

  // set size
  if (ftruncate(Fd, Size) == -1) {
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
      mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0));
  if (PoolData == MAP_FAILED) {
    int SavedErrno = errno;
    close(Fd);
    Fd = -1;
    PoolData = nullptr;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to map shared memory");
  }
  PoolSize = Size;
}

uint32_t Display::wlGetRegistry() {
  Utils::Buffer Msg;
  Msg.writeUint(DisplayObjectId);
  Msg.writeUint(Opcode::WlDisplayGetRegistry);

  // message size (part of header)
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // argument to get_registry
  CurrentId++;
  Msg.writeUint(CurrentId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), MSG_DONTWAIT);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send get_registry message");

  return CurrentId;
}

uint32_t Display::wlRegistryBind(uint32_t Name, std::string_view Interface,
                                 uint32_t InterfaceLen, uint32_t Version) {
  Utils::Buffer Msg;
  Msg.writeUint(State.WlRegistry);
  Msg.writeUint(Opcode::WlRegistryBind);

  // calculate message size
  uint32_t PaddedInterfaceLen = Utils::roundUp4(InterfaceLen);
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(Name) + sizeof(InterfaceLen) +
                              PaddedInterfaceLen + sizeof(Version) +
                              sizeof(CurrentId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write bind arguments
  Msg.writeUint(Name);
  Msg.writeString(Interface);
  Msg.writeUint(Version);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // verify buffer size is aligned
  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send registry bind message");

  return CurrentId;
}

uint32_t Display::wlCompositorCreateSurface() {
  assert(State.WlCompositor > 0);

  Utils::Buffer Msg;
  Msg.writeUint(State.WlCompositor);
  Msg.writeUint(Opcode::WlCompositorCreateSurface);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send compositor create_surface message");

  return CurrentId;
}

// TODO: revisit this method
uint32_t Display::wlShmCreatePool() {
  assert(SharedMemory.PoolSize > 0);

  Utils::Buffer Msg;
  Msg.writeUint(State.WlShm);
  Msg.writeUint(Opcode::WlShmCreatePool);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentId) + sizeof(SharedMemory.PoolSize);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // write pool size
  Msg.writeUint(SharedMemory.PoolSize);

  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send the file descriptor as ancillary data
  char Buf[CMSG_SPACE(sizeof(SharedMemory.Fd))]{};

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
  Cmsg->cmsg_len = CMSG_LEN(sizeof(SharedMemory.Fd));

  *reinterpret_cast<int *>(CMSG_DATA(Cmsg)) = SharedMemory.Fd;
  SocketMsg.msg_controllen = CMSG_SPACE(sizeof(SharedMemory.Fd));

  if (sendmsg(Fd, &SocketMsg, 0) == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_shm create_pool message");

  return CurrentId;
}

uint32_t Display::wlXdgWmBaseGetXdgSurface() {
  assert(State.XdgWmBase > 0);
  assert(State.WlSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(State.XdgWmBase);
  Msg.writeUint(Opcode::XdgWmBaseGetXdgSurface);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentId) + sizeof(State.WlSurface);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // write wl_surface argument
  Msg.writeUint(State.WlSurface);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(
        errno, std::generic_category(),
        "failed to send xdg_wm_base get_xdg_surface message");

  return CurrentId;
}

uint32_t Display::wlShmPoolCreateBuffer() {
  assert(State.WlShmPool > 0);

  Utils::Buffer Msg;
  Msg.writeUint(State.WlShmPool);
  Msg.writeUint(Opcode::WlShmPoolCreateBuffer);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentId) + sizeof(uint32_t) * 5;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // write buffer parameters
  uint32_t Offset = 0;
  Msg.writeUint(Offset);
  Msg.writeUint(SharedMemory.Width);
  Msg.writeUint(SharedMemory.Height);
  Msg.writeUint(SharedMemory.Stride);
  Msg.writeUint(Format::Xrgb8888);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_shm_pool create_buffer message");

  return CurrentId;
}

void Display::wlSurfaceAttach() {
  assert(State.WlSurface > 0);
  assert(State.WlBuffer > 0);

  Utils::Buffer Msg;
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

void Display::wlSurfaceCommit() {
  assert(State.WlSurface > 0);

  Utils::Buffer Msg;
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

uint32_t Display::wlXdgSurfaceGetToplevel() {
  assert(State.XdgSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(State.XdgSurface);
  Msg.writeUint(Opcode::XdgSurfaceGetToplevel);

  // calculate message size
  uint16_t MsgSizeAnnounced = HeaderSize + sizeof(CurrentId);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send xdg_surface get_toplevel message");

  return CurrentId;
}

void Display::wlXdgWmBasePong(uint32_t Ping) {
  assert(State.XdgWmBase > 0);
  assert(State.WlSurface > 0);

  Utils::Buffer Msg;
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

void Display::wlXdgSurfaceAckConfigure(uint32_t Configure) {
  assert(State.XdgSurface > 0);

  Utils::Buffer Msg;
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

void Display::handleMessage(Utils::Buffer &Msg) {
  assert(Msg.size() >= 8);

  // read header
  uint32_t ObjectId = Msg.readUint<uint32_t>();
  assert(ObjectId <= CurrentId);

  uint16_t Opcode = Msg.readUint<uint16_t>();
  uint16_t MsgSizeAnnounced = Msg.readUint<uint16_t>();
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);

  // verify message size
  uint32_t HeaderSize =
      sizeof(ObjectId) + sizeof(Opcode) + sizeof(MsgSizeAnnounced);
  uint32_t MsgSize = HeaderSize + (Msg.size() - Msg.pos());
  assert(MsgSizeAnnounced <= MsgSize);

  // dispatch based on object and opcode
  if (ObjectId == State.WlRegistry && Opcode == WlRegistryEventGlobal) {
    handleRegistryGlobal(Msg, MsgSizeAnnounced);
  } else if (ObjectId == DisplayObjectId && Opcode == WlDisplayErrorEvent) {
    handleDisplayError(Msg);
  } else {
    // TODO: add more event handlers
  }
}

void Display::handleRegistryGlobal(Utils::Buffer &Msg,
                                   uint16_t MsgSizeAnnounced) {
  uint32_t Name = Msg.readUint<uint32_t>();
  uint32_t InterfaceLen = Msg.readUint<uint32_t>();
  std::string Interface = Msg.readString(InterfaceLen);
  uint32_t Version = Msg.readUint<uint32_t>();

  // verify message size
  uint32_t PaddedInterfaceLen = Utils::roundUp4(Interface.size() + 1);
  assert(MsgSizeAnnounced == HeaderSize + sizeof(Name) + sizeof(uint32_t) +
                                 PaddedInterfaceLen + sizeof(Version));

  // bind interfaces we need
  if (Interface == "wl_shm") {
    State.WlShm =
        wlRegistryBind(Name, Interface, Interface.size() + 1, Version);
  } else if (Interface == "xdg_wm_base") {
    State.XdgWmBase =
        wlRegistryBind(Name, Interface, Interface.size() + 1, Version);
  } else if (Interface == "wl_compositor") {
    State.WlCompositor =
        wlRegistryBind(Name, Interface, Interface.size() + 1, Version);
  }
}

void Display::handleDisplayError(Utils::Buffer &Msg) {
  uint32_t TargetObjectId = Msg.readUint<uint32_t>();
  uint32_t Code = Msg.readUint<uint32_t>();
  uint32_t ErrorLen = Msg.readUint<uint32_t>();
  std::string Error = Msg.readString(ErrorLen);

  throw std::runtime_error("fatal wayland error: target_object_id=" +
                           std::to_string(TargetObjectId) +
                           " code=" + std::to_string(Code) + " error=" + Error);
}
} // namespace Wayland

int main() {
  try {
    Wayland::Display Display;
  } catch (const std::exception &Exception) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}