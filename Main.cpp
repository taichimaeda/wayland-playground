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

static constexpr uint32_t WaylandDisplayObjectId = 1;
static constexpr uint16_t WaylandWlRegistryEventGlobal = 0;
static constexpr uint16_t WaylandShmPoolEventFormat = 0;
static constexpr uint16_t WaylandWlBufferEventRelease = 0;
static constexpr uint16_t WaylandXdgWmBaseEventPing = 0;
static constexpr uint16_t WaylandXdgToplevelEventConfigure = 0;
static constexpr uint16_t WaylandXdgToplevelEventClose = 1;
static constexpr uint16_t WaylandXdgSurfaceEventConfigure = 0;
static constexpr uint16_t WaylandWlDisplayGetRegistryOpcode = 1;
static constexpr uint16_t WaylandWlRegistryBindOpcode = 0;
static constexpr uint16_t WaylandWlCompositorCreateSurfaceOpcode = 0;
static constexpr uint16_t WaylandXdgWmBasePongOpcode = 3;
static constexpr uint16_t WaylandXdgSurfaceAckConfigureOpcode = 4;
static constexpr uint16_t WaylandWlShmCreatePoolOpcode = 0;
static constexpr uint16_t WaylandXdgWmBaseGetXdgSurfaceOpcode = 2;
static constexpr uint16_t WaylandWlShmPoolCreateBufferOpcode = 0;
static constexpr uint16_t WaylandWlSurfaceAttachOpcode = 1;
static constexpr uint16_t WaylandXdgSurfaceGetToplevelOpcode = 1;
static constexpr uint16_t WaylandWlSurfaceCommitOpcode = 6;
static constexpr uint16_t WaylandWlDisplayErrorEvent = 0;
static constexpr uint32_t WaylandFormatXrgb8888 = 1;
static constexpr uint32_t WaylandHeaderSize = 8;
static constexpr uint32_t ColorChannels = 4;

class WaylandDisplay {
public:
  void connect();

private:
  int Fd = -1;
  std::string XdgRuntimeDir;
  std::string WaylandDisplayName;
};

void WaylandDisplay::connect() {
  // get env vars
  char *XdgRuntimeDirEnv = std::getenv("XDG_RUNTIME_DIR");
  if (!XdgRuntimeDirEnv)
    throw std::runtime_error("XDG_RUNTIME_DIR not set");
  XdgRuntimeDir = std::string(XdgRuntimeDirEnv);

  char *WaylandDisplayEnv = std::getenv("WAYLAND_DISPLAY");
  WaylandDisplayName =
      WaylandDisplayEnv ? std::string(WaylandDisplayEnv) : "wayland-0";

  // prepare socket path
  std::string SocketPath = XdgRuntimeDir + "/" + WaylandDisplayName;
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

namespace Utils {

static constexpr size_t roundUp4(size_t N) { return (N + 3) & ~size_t(3); }

class Buffer {
public:
  explicit Buffer() : Data{}, Position{0} {}
  explicit Buffer(const std::vector<char> &Data) : Data{Data}, Position{0} {}

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  void write(T Value) {
    assert(Position % alignof(T) == 0);

    size_t OldSize = Data.size();
    Data.resize(OldSize + sizeof(T));
    std::memcpy(Data.data() + OldSize, &Value, sizeof(T));
    Position += sizeof(T);
  }

  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  T read() {
    assert(Position + sizeof(T) <= Data.size());
    assert(Position % alignof(T) == 0);

    T Value;
    std::memcpy(&Value, Data.data() + Position, sizeof(T));
    Position += sizeof(T);
    return Value;
  }

  void writeString(std::string_view Str) {
    // write length first for Wayland protocol
    write(static_cast<uint32_t>(Str.size()));

    size_t PaddedLen = roundUp4(Str.size());
    size_t OldSize = Data.size();
    Data.resize(OldSize + PaddedLen, '\0');
    std::memcpy(Data.data() + Position, Str.data(), Str.size());
    Position += PaddedLen;
  }

  std::string readString() {
    uint32_t Len = read<uint32_t>();
    assert(Position + Len <= Data.size());

    std::string Result{Data.data() + Position, Len};
    Position += roundUp4(Len);
    return Result;
  }

  size_t size() const { return Data.size(); }
  size_t position() const { return Position; }

private:
  std::vector<char> Data;
  size_t Position;
};

} // namespace Utils

int main() {
  try {
    WaylandDisplay Display;
  } catch (const std::exception &Exception) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}