#ifndef CMDLINE_H
#define CMDLINE_H

/* A console line into words: runs without spaces, or "quoted" / 'quoted'
   runs with them. A word longer than CMD_WORD-1 is cut. Pure. */
#define CMD_WORD 400
#define CMD_MAX 6

int cmd_split(const char *line, char words[][CMD_WORD], int max);

#endif /* CMDLINE_H */
