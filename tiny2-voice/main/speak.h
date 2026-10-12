#ifndef SPEAK_H
#define SPEAK_H
/* Text to ElevenLabs, collected, made louder, then played out of the amplifier pin.
   200 ok; 0 key missing; -2 voice missing; otherwise the HTTP status, -1 no connection. */
int speak(const char *text);
void speak_tone(void);       /* a short two-note chime, to test the speaker without the network */
#endif /* SPEAK_H */
