/* ctype.h - freestanding ASCII character classification for the Sweep KAPA.
 * ASCII-only by design: no locale semantics, exactly the functions the
 * freestanding engine calls (see sweep_freestanding_libc.h declaration). */
#ifndef SWEEP_FS_CTYPE_H
#define SWEEP_FS_CTYPE_H

int tolower(int c);
int toupper(int c);
int isalpha(int c);
int isalnum(int c);
int isdigit(int c);
int isspace(int c);
int isctrl(int c);

#endif /* SWEEP_FS_CTYPE_H */
