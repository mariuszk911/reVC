#ifndef __GTA_AMIGA_BOOT_H__
#define __GTA_AMIGA_BOOT_H__

/*
 * AmigaOS boot / platform hook prototypes (implemented in
 * src/skel/amigaos/amigaos.cpp). Shared files include this instead of
 * re-declaring the `amiga_boot_*` C hooks ad hoc, so the signatures live in one
 * place (W-1).
 *
 * Off-AmigaOS every hook is an inline no-op, so a shared file that references
 * one still compiles and links on the PC reference build. On AmigaOS the
 * declarations match the C-linkage definitions in amigaos.cpp.
 */

#ifdef AMIGAOS

extern "C" void amiga_boot_log(const char *s);
extern "C" unsigned int amiga_boot_ms(void);
extern "C" unsigned int amiga_boot_us(void);
extern "C" void amiga_boot_poll_events(void);
extern "C" int amiga_boot_should_quit(void);
extern "C" void amiga_boot_yield(void);
extern "C" unsigned int amiga_boot_avail(void);
extern "C" int amiga_boot_async_stream(void);
extern "C" int amiga_boot_vertexarray(void);
extern "C" int amiga_boot_texuniq(void);
extern "C" int amiga_boot_lockarrays(void);
extern "C" int amiga_boot_lockwin(void);
extern "C" int amiga_boot_texmemlog(void);
extern "C" int amiga_boot_randomsplash(void);
extern "C" int amiga_boot_clearcolor(void);
extern "C" int amiga_boot_streamstats(void);
extern "C" int amiga_boot_streamstats_frames(void);
extern "C" int amiga_boot_streamstats_slow_ms(void);

#else /* !AMIGAOS */

inline void amiga_boot_log(const char *) {}
inline unsigned int amiga_boot_ms(void) { return 0u; }
inline unsigned int amiga_boot_us(void) { return 0u; }
inline void amiga_boot_poll_events(void) {}
inline int amiga_boot_should_quit(void) { return 0; }
inline void amiga_boot_yield(void) {}
inline unsigned int amiga_boot_avail(void) { return 0u; }
inline int amiga_boot_async_stream(void) { return 0; }
inline int amiga_boot_vertexarray(void) { return 0; }
inline int amiga_boot_texuniq(void) { return 0; }
inline int amiga_boot_lockarrays(void) { return 0; }
inline int amiga_boot_lockwin(void) { return 0; }
inline int amiga_boot_texmemlog(void) { return 0; }
inline int amiga_boot_randomsplash(void) { return 0; }
inline int amiga_boot_clearcolor(void) { return 0; }
inline int amiga_boot_streamstats(void) { return 0; }
inline int amiga_boot_streamstats_frames(void) { return 0; }
inline int amiga_boot_streamstats_slow_ms(void) { return 0; }

#endif /* AMIGAOS */

#endif /* __GTA_AMIGA_BOOT_H__ */
