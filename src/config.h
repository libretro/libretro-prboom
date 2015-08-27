/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Define to be the path where Doom WADs are stored */
#define DOOMWADDIR "/usr/local/share/games/doom"

#define SURFACE_PIXEL_DEPTH 2
extern int SCREENWIDTH;
extern int SCREENHEIGHT;
#define SCREENPITCH (SCREENWIDTH*SURFACE_PIXEL_DEPTH)

#define SURFACE_WIDTH SCREENWIDTH
#define SURFACE_BYTE_PITCH SCREENPITCH
#define SURFACE_SHORT_PITCH SCREENWIDTH
#define SURFACE_INT_PITCH (SCREENWIDTH/2)

/* Define to 1 if you have the <asm/byteorder.h> header file. */
#ifndef __CELLOS_LV2__
#define HAVE_ASM_BYTEORDER_H 1
#endif

#ifdef GEKKO
#define HAVE_STRLWR
#endif

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

#ifdef HAVE_NET

/* Define to 1 if you have the `inet_aton' function. */
#define HAVE_INET_ATON 1

/* Define to 1 if you have the `inet_ntop' function. */
#define HAVE_INET_NTOP 1

/* Define to 1 if you have the `inet_pton' function. */
#define HAVE_INET_PTON 1

#endif

/* Define to 1 if you have the `snprintf' function. */
#define HAVE_SNPRINTF 1

/* Define to 1 if you have the `vsnprintf' function. */
#ifndef _XBOX1
#define HAVE_VSNPRINTF 1
#endif

/* Name of package */
#define PACKAGE "prboom"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "prboom"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "prboom 2.5.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "prboom"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "2.5.0"

/* Set to the attribute to apply to struct definitions to make them packed */
#ifdef __MINGW32__
/* Use gcc_struct to work around http://gcc.gnu.org/PR52991 */
#define PACKEDATTR __attribute__((packed, gcc_struct))
#else
#define PACKEDATTR __attribute__((packed))
#endif

/* Version number of package */
#define VERSION "2.5.0"

/* Define this to perform id checks on zone blocks, to detect corrupted and
   illegally freed blocks */
#define ZONEIDCHECK 1

/* Define to strcasecmp, if we have it */
#define stricmp strcasecmp

/* Define to strncasecmp, if we have it */
#define strnicmp strncasecmp

#ifdef _MSC_VER
#define snprintf _snprintf
#endif
