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
  void write(T Value) {
    assert(Pos % alignof(T) == 0);

    size_t OldSize = Data.size();
    Data.resize(OldSize + sizeof(T));
    std::memcpy(Data.data() + OldSize, &Value, sizeof(T));
    Pos += sizeof(T);
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  T read() {
    assert(Pos + sizeof(T) <= Data.size());
    assert(Pos % alignof(T) == 0);

    T Value;
    std::memcpy(&Value, Data.data() + Pos, sizeof(T));
    Pos += sizeof(T);
    return Value;
  }

  void writeString(std::string_view Str) {
    // write length first for Wayland protocol
    write(static_cast<uint32_t>(Str.size()));

    size_t PaddedLen = roundUp4(Str.size());
    size_t OldSize = Data.size();
    Data.resize(OldSize + PaddedLen, '\0');
    std::memcpy(Data.data() + Pos, Str.data(), Str.size());
    Pos += PaddedLen;
  }

  std::string readString() {
    uint32_t Len = read<uint32_t>();
    assert(Pos + Len <= Data.size());

    std::string Result{Data.data() + Pos, Len};
    Pos += roundUp4(Len);
    return Result;
  }

  const char *data() const { return Data.data(); }
  size_t size() const { return Data.size(); }
  size_t position() const { return Pos; }

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
  void connect();
  void getRegistry();
  void createSharedMemoryFile(size_t Size);

private:
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

void Display::getRegistry() {
  Utils::Buffer Msg;
  Msg.write(DisplayObjectId);
  Msg.write(WlDisplayGetRegistryOpcode);

  // message size (part of header)
  uint16_t MsgAnnouncedSize = HeaderSize + sizeof(CurrentId);
  assert(Utils::roundUp4(MsgAnnouncedSize) == MsgAnnouncedSize);
  Msg.write(MsgAnnouncedSize);

  // argument to get_registry
  CurrentId++;
  Msg.write(CurrentId);

  // send message
  ssize_t Sent = send(Fd, Msg.data(), Msg.size(), MSG_DONTWAIT);
  if (static_cast<size_t>(Sent) != Msg.size())
    throw std::system_error(errno, std::generic_category(),
                            "failed to send get_registry message");
}

void Display::createSharedMemoryFile(size_t Size) {
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
} // namespace Wayland

int main() {
  try {
    Wayland::Display Display;
  } catch (const std::exception &Exception) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}