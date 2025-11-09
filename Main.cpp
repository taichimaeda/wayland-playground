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

static std::string generateRandomString(size_t Len) {
  std::string Result;
  Result.reserve(Len);
  for (size_t I = 0; I < Len; ++I) {
    Result += static_cast<char>(static_cast<double>(std::rand()) /
                                    static_cast<double>(RAND_MAX) * 26 +
                                'a');
  }
  return Result;
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

  // TODO: check if this handles null terminator correctly
  void writeString(std::string_view Str) {
    // write length first as per Wayland protocol
    writeUint(static_cast<uint32_t>(Str.size()));

    size_t PaddedLen = roundUp4(Str.size());
    size_t OldSize = Data.size();
    Data.resize(OldSize + PaddedLen, '\0');
    std::memcpy(Data.data() + Pos, Str.data(), Str.size());
    Pos += PaddedLen;
  }

  // TODO: check if this handles null terminator correctly
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

static constexpr uint32_t DisplayObjectId = 1;
static constexpr uint16_t WlRegistryEventGlobal = 0;
static constexpr uint16_t ShmPoolEventFormat = 0;
static constexpr uint16_t WlBufferEventRelease = 0;
static constexpr uint16_t XdgWmBaseEventPing = 0;
static constexpr uint16_t XdgToplevelEventConfigure = 0;
static constexpr uint16_t XdgToplevelEventClose = 1;
static constexpr uint16_t XdgSurfaceEventConfigure = 0;
static constexpr uint16_t WlDisplayGetRegistryOpcode = 1;
static constexpr uint16_t WlRegistryBindOpcode = 0;
static constexpr uint16_t WlCompositorCreateSurfaceOpcode = 0;
static constexpr uint16_t XdgWmBasePongOpcode = 3;
static constexpr uint16_t XdgSurfaceAckConfigureOpcode = 4;
static constexpr uint16_t WlShmCreatePoolOpcode = 0;
static constexpr uint16_t XdgWmBaseGetXdgSurfaceOpcode = 2;
static constexpr uint16_t WlShmPoolCreateBufferOpcode = 0;
static constexpr uint16_t WlSurfaceAttachOpcode = 1;
static constexpr uint16_t XdgSurfaceGetToplevelOpcode = 1;
static constexpr uint16_t WlSurfaceCommitOpcode = 6;
static constexpr uint16_t WlDisplayErrorEvent = 0;
static constexpr uint32_t FormatXrgb8888 = 1;
static constexpr uint32_t HeaderSize = 8;
static constexpr uint32_t ColorChannels = 4;

enum class Status {
  None,
  SurfaceAckedConfigure,
  SurfaceAttached,
};

class Display {
public:
  // TODO: add destructor
  // TODO: prohibit copy/move
  void connect();
  void allocate(size_t Size);

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

  // TODO: extract into separate class
  // Wayland objects
  uint32_t WlRegistry{};
  uint32_t WlShm{};
  uint32_t WlShmPool{};
  uint32_t WlBuffer{};
  uint32_t XdgWmBase{};
  uint32_t XdgSurface{};
  uint32_t WlCompositor{};
  uint32_t WlSurface{};
  uint32_t XdgToplevel{};
  Status CurrentStatus{Status::None};

  // TODO: extract into separate class
  // shared memory state
  uint32_t Stride{};
  uint32_t Width{};
  uint32_t Height{};
  uint32_t ShmPoolSize{};
  int ShmFd{-1};
  uint8_t *ShmPoolData{nullptr};
};

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

void Display::allocate(size_t Size) {
  // create shared memory file
  // O_RDWR for read/write
  // O_EXCL | O_CREAT to ensure a new file is always created
  std::string Name = "/" + Utils::generateRandomString(253);
  ShmFd = shm_open(Name.c_str(), O_RDWR | O_EXCL | O_CREAT, 0600);
  if (ShmFd == -1)
    throw std::system_error(errno, std::generic_category(),
                            "failed to create shared memory");

  // unlink immediately for clean up on exit
  if (shm_unlink(Name.c_str()) == -1) {
    int SavedErrno = errno;
    close(ShmFd);
    ShmFd = -1;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to unlink shared memory");
  }

  // set size
  if (ftruncate(ShmFd, Size) == -1) {
    int SavedErrno = errno;
    close(ShmFd);
    ShmFd = -1;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to truncate shared memory");
  }

  // map memory
  // PROT_READ | PROT_WRITE for read/write
  // MAP_SHARED for sharing with compositor process
  ShmPoolData = static_cast<uint8_t *>(
      mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, ShmFd, 0));
  if (ShmPoolData == MAP_FAILED) {
    int SavedErrno = errno;
    close(ShmFd);
    ShmFd = -1;
    ShmPoolData = nullptr;
    throw std::system_error(SavedErrno, std::generic_category(),
                            "failed to map shared memory");
  }

  ShmPoolSize = Size;
}

uint32_t Display::wlGetRegistry() {
  Utils::Buffer Msg;
  Msg.writeUint(DisplayObjectId);
  Msg.writeUint(WlDisplayGetRegistryOpcode);

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
  Msg.writeUint(WlRegistry);
  Msg.writeUint(WlRegistryBindOpcode);

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
  assert(WlCompositor > 0);

  Utils::Buffer Msg;
  Msg.writeUint(WlCompositor);
  Msg.writeUint(WlCompositorCreateSurfaceOpcode);

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
  assert(ShmPoolSize > 0);

  Utils::Buffer Msg;
  Msg.writeUint(WlShm);
  Msg.writeUint(WlShmCreatePoolOpcode);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentId) + sizeof(ShmPoolSize);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // write pool size
  Msg.writeUint(ShmPoolSize);

  assert(Msg.size() == Utils::roundUp4(Msg.size()));

  // send the file descriptor as ancillary data
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

  return CurrentId;
}

uint32_t Display::wlXdgWmBaseGetXdgSurface() {
  assert(XdgWmBase > 0);
  assert(WlSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(XdgWmBase);
  Msg.writeUint(XdgWmBaseGetXdgSurfaceOpcode);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(CurrentId) + sizeof(WlSurface);
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // allocate new object id and write it
  CurrentId++;
  Msg.writeUint(CurrentId);

  // write wl_surface argument
  Msg.writeUint(WlSurface);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(
        errno, std::generic_category(),
        "failed to send xdg_wm_base get_xdg_surface message");

  return CurrentId;
}

uint32_t Display::wlShmPoolCreateBuffer() {
  assert(WlShmPool > 0);

  Utils::Buffer Msg;
  Msg.writeUint(WlShmPool);
  Msg.writeUint(WlShmPoolCreateBufferOpcode);

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
  Msg.writeUint(Width);
  Msg.writeUint(Height);
  Msg.writeUint(Stride);
  Msg.writeUint(FormatXrgb8888);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), 0);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send wl_shm_pool create_buffer message");

  return CurrentId;
}

void Display::wlSurfaceAttach() {
  assert(WlSurface > 0);
  assert(WlBuffer > 0);

  Utils::Buffer Msg;
  Msg.writeUint(WlSurface);
  Msg.writeUint(WlSurfaceAttachOpcode);

  // calculate message size
  uint16_t MsgSizeAnnounced =
      HeaderSize + sizeof(WlBuffer) + sizeof(uint32_t) * 2;
  assert(Utils::roundUp4(MsgSizeAnnounced) == MsgSizeAnnounced);
  Msg.writeUint(MsgSizeAnnounced);

  // write buffer and position
  Msg.writeUint(WlBuffer);
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
  assert(WlSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(WlSurface);
  Msg.writeUint(WlSurfaceCommitOpcode);

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
  assert(XdgSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(XdgSurface);
  Msg.writeUint(XdgSurfaceGetToplevelOpcode);

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
  assert(XdgWmBase > 0);
  assert(WlSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(XdgWmBase);
  Msg.writeUint(XdgWmBasePongOpcode);

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
  assert(XdgSurface > 0);

  Utils::Buffer Msg;
  Msg.writeUint(XdgSurface);
  Msg.writeUint(XdgSurfaceAckConfigureOpcode);

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
  if (ObjectId == WlRegistry && Opcode == WlRegistryEventGlobal) {
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
    WlShm = wlRegistryBind(Name, Interface, Interface.size() + 1, Version);
  } else if (Interface == "xdg_wm_base") {
    XdgWmBase = wlRegistryBind(Name, Interface, Interface.size() + 1, Version);
  } else if (Interface == "wl_compositor") {
    WlCompositor =
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