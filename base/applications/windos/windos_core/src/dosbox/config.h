#define VERSION "0.74-3"

/* Define to 1 to enable internal debugger, requires libcurses */
#define C_DEBUG 0

/* Define to 1 to enable screenshots, requires libpng */
#define C_SSHOT 0

/* Define to 1 to use opengl display output support */
#define C_OPENGL 0

/* Define to 1 to enable internal modem support, requires SDL_net */
#define C_MODEM 0

/* Define to 1 to enable IPX networking support, requires SDL_net */
#define C_IPX 0

/* Enable some heavy debugging options */
#define C_HEAVY_DEBUG 0

/* The type of cpu this host has */
#ifdef _WIN64
#define C_TARGETCPU X86_64
#else
#define C_TARGETCPU X86
#endif

/* Define to 1 to use x86 dynamic cpu core */
#define C_DYNAMIC_X86 0

/* Define to 1 to use recompiling cpu core. Can not be used together with the dynamic-x86 core */
#define C_DYNREC 0

/* Enable memory function inlining in */
#define C_CORE_INLINE 0

/* Enable the FPU module (the portable C++ one: C_FPU_X86 stays 0). DOS
   games and programs that need a math coprocessor rely on it. */
#define C_FPU 1

/* Define to 1 to use a x86 assembly fpu core */
#define C_FPU_X86 0

/* Define to 1 to use a unaligned memory access */
#define C_UNALIGNED_MEMORY 1

/* environ is defined */
#define ENVIRON_INCLUDED 1

/* environ can be linked */
#define ENVIRON_LINKED 1

/* Define to 1 if you have the <ddraw.h> header file. */
#define HAVE_DDRAW_H 0

/* Define to 1 if you want serial passthrough support (Win32 only). */
#define C_DIRECTSERIAL 0

#define GCC_ATTRIBUTE(x) /* attribute not supported */
#define GCC_UNLIKELY(x) (x)
#define GCC_LIKELY(x) (x)

#define INLINE __forceinline
#define DB_FASTCALL __fastcall

#if defined(_MSC_VER) && (_MSC_VER >= 1400) 
#pragma warning(disable : 4996) 
#endif

typedef         double		Real64;
/* The internal types */
typedef  unsigned char		Bit8u;
typedef    signed char		Bit8s;
typedef unsigned short		Bit16u;
typedef   signed short		Bit16s;
typedef  unsigned long		Bit32u;
typedef    signed long		Bit32s;
typedef unsigned __int64	Bit64u;
typedef   signed __int64	Bit64s;
/* Bitu/Bits hold pointers in places (the VGA and OPL code), so they are
   pointer-sized, as in DOSBox's own 64-bit builds. */
#ifdef _WIN64
typedef unsigned __int64	Bitu;
typedef   signed __int64	Bits;
#else
typedef unsigned int		Bitu;
typedef signed int			Bits;
#endif

