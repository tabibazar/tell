/* mp4mux: feed it a real Annex-B baseline stream frame by frame (as the ESP32
   encoder hands them out) and check ffprobe/ffmpeg accept the MP4. The stream
   comes from ffmpeg/libx264 (host_tests/renders/mp4mux/in.h264); this only
   splits it into access units and calls mp4mux. Usage: test_mp4mux in out */
#include "../spy/main/mp4mux.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 2;
    fseek(in, 0, SEEK_END); long n = ftell(in); fseek(in, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, in) != (size_t)n) return 2;
    fclose(in);
    FILE *out = fopen(argv[2], "wb+");
    mp4mux_t *m = mp4mux_open(out, 640, 480, 10);
    /* Access units: a new one starts at each slice NAL (one slice per frame),
       with any SPS/PPS/SEI before it going with it. */
    long au = 0; int frames = 0; long i = 0; int seen_slice = 0;
    while (i + 3 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int t = d[i + 3] & 0x1F;
            long sc = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            if ((t == 1 || t == 5 || t == 7 || t == 9) && seen_slice && (t == 7 || t == 9 || t == 1 || t == 5)) {
                /* close the previous AU before an SPS/AUD or another slice */
                int key = 0;
                for (long k = au; k + 3 < sc; k++) if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1 && (d[k + 3] & 0x1F) == 5) key = 1;
                if (!mp4mux_add(m, d + au, (size_t)(sc - au), key)) { printf("FAIL add %d\n", frames); return 1; }
                frames++; au = sc; seen_slice = 0;
            }
            if (t == 1 || t == 5) seen_slice = 1;
            i += 3;
        } else i++;
    }
    int key = 0;
    for (long k = au; k + 3 < n; k++) if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1 && (d[k + 3] & 0x1F) == 5) key = 1;
    if (!mp4mux_add(m, d + au, (size_t)(n - au), key)) { printf("FAIL last\n"); return 1; }
    frames++;
    if (!mp4mux_close(m)) { printf("FAIL close\n"); return 1; }
    fclose(out);
    printf("muxed %d frames\n", frames);
    return 0;
}
