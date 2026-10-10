/* libusb settings for MinGW-w64 builds. libusb only ships a hand-written config.h for MSVC
   (msvc/config.h, which refuses other compilers); this sets the same options. */

#define DEFAULT_VISIBILITY /**/
#define ENABLE_LOGGING 1
#define PLATFORM_WINDOWS 1
#define PRINTF_FORMAT(a, b) /**/
