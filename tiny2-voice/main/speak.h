#ifndef SPEAK_H
#define SPEAK_H
/* Text to ElevenLabs, the speech streamed out of the PDM pin as it arrives.
   200 ok; 0 key missing; -2 voice missing; otherwise the HTTP status, -1 no connection. */
int speak(const char *text);
void speak_tone(void);       /* a short two-note chime, to test the speaker without the network */
#endif /* SPEAK_H */
