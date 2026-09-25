# speaker (Waveshare ESP32-S3-AUDIO-Board): reconciled pinout research

Two independent researchers (schematic v1.1; factory demo + xiaozhi code) and a reconcile pass, 2026-09-25.

## gpio

Sources: schematic v1.1 (the pin table, read by text-layer coordinates, plus the crops), the wiki pinout, factory_01 bsp_board.h, xiaozhi config.h and the Arduino headers. They all agree unless a line says otherwise.

I2S. There is one clock set. The ESP32 is master and both codecs are slaves, so TX and RX must run at the same sample rate.
- MCLK = GPIO12, to ES8311 pin 2 and ES7210 pin 5.
- BCLK/SCLK = GPIO13, to ES8311 pin 6 and ES7210 pin 9.
- WS/LRCK = GPIO14, to ES8311 pin 8 and ES7210 pin 10.
- DOUT (speaker) = GPIO16, net I2S_DSDIN, to ES8311 DSDIN (pin 9).
- DIN (mics) = GPIO15, net I2S_ASDOUT, from ES7210 SDOUT1/TDMOUT (pin 11) through R47 51R.
  - ES7210 SDOUT2 (pin 12) is not connected (R48 NC). With more than 2 channels, everything must come out as TDM on SDOUT1.
- The ES8311's own ADC is unused: its ASDOUT (pin 7) and MIC1P/N are stubs.
- The factory demo uses I2S_NUM_1 and xiaozhi uses I2S_NUM_0. Either port works.

I2C:
- SDA = GPIO11, SCL = GPIO10, port 0.
- It is one bus. The nets ESP32_, EXIO_, RTC_, TP_ and TWI_ (camera SCCB) all land on these two pins.
- Pull-ups: R23/R61 2.2k (codec sheet), R16/R17 10k (TCA sheet) and R55/R57 4.7k (camera sheet). Together that is about 1.3k.

WS2812 ring:
- Data = GPIO38 (net LED_DIN), driven directly over RMT, not through the expander.
- 7 x WS2812B-0807, powered from 3V3.
- Chain (verified on the ledsL/ledsR crops): GPIO38 -> U9 (index 0) -> U19 -> U14 -> U13 -> U12 -> U11 -> U10 (index 6). U10's DOUT dangles.
- Use a count of 7, not xiaozhi's 6.
- Colour order: default to RGB (see disagreements).
- Where index 0 sits on the ring is not in any source. Find out by lighting indices 0..6 one at a time.

Keys:
- BOOT = Key4 on GPIO0: R27 10k pull-up, C23 NC, active low, a strapping pin.
- RESET = Key5 on CHIP_PU (R34 10k, C25 1uF). Firmware cannot read it.
- Key1/Key2/Key3 = TCA9555 EXIO9/10/11 (P11/P12/P13), with 10k pull-ups R7/R44/R77, active low.
  - They must be polled over I2C; the factory demo polls every 100 ms.
  - Factory mapping: Key1 (EXIO9) = volume up / long press previous; Key2 (EXIO10) = play-pause / long press stop; Key3 (EXIO11) = volume down / long press next.

SD card (TF-07F), 1-bit SDMMC:
- CLK = GPIO40, CMD = GPIO42, D0 = GPIO41.
- D3/CS = TCA EXIO3 (10k pull-up R65, through R66 0R).
- D1/D2 have only 10k pull-ups and are not wired to the MCU.
- No card-detect to the MCU: the CD pin is strapped by jumper R26.
- The factory demo uses SDMMC width 1 with D3 NC.
- SPI alternative: SCK 40, MOSI 42, MISO 41, CS = EXIO3.

Battery ADC. Net BAT_ADC = Vbat x 100k/(200k+100k) = Vbat/3 (R2 200k 1%, R11 100k 1%, C10 100n), taken after the SW1/Q2 battery switch. It is NOT connected by default.
- Re-cropped as /private/tmp/speaker/crops/batj.png:
  - R53: common pin = GPIO1; 0R to CAM_HREF; its other pad (NC) goes to R78.
  - R78: common pin = BAT_ADC; 0R to R53's NC pad; NC to GPIO6.
  - So BAT_ADC dead-ends at an empty pad.
- To enable it, either:
  - move R53's 0R to its NC position, so GPIO1 (ADC1_CH0) reads Vbat/3 and CAM_HREF is lost; this is what the schematic note describes; or
  - move R78's 0R to its NC position, so BAT_ADC goes to GPIO6 (ADC1_CH5; GPIO6 is used only by a QSPI LCD).
- The Arduino demo's BAT_ADC_PIN 8 is wrong: GPIO8 is LCD MISO/SDA1 on this board.

Other pins:
- USB D-/D+ = GPIO19/20 (22R series). UART0 TX = 43, RX = 44.
- LCD FPC: CS 3, SCLK 4, BL_PWM 5, DC/SDA2 7, MISO/SDA1 8, MOSI/SDA0 9, SDA3 6.
- Camera: D0-D7 = 2, 17, 18, 39, 45, 46, 47, 48; VSYNC 21; HREF 1. PCLK/XCLK go through the FSUSB42UMX mux (EXIO6): GPIO44/43 when high, GPIO19/20 when low.
- GPIO33-37 are used by the octal PSRAM (ESP32-S3R8). GPIO46 has a 10k pull-down, R76.
- Free with no LCD or camera fitted: 1-9, 17, 18, 21, 39, 45-48. Watch the strapping pins 3, 45 and 46.
- The P1 2x8 header carries GPIO3-9, IO19/20, SCL/SDA, EXIO0-2, 3V3, 5V and GND.
- There is no firmware power-hold pin.

## i2c

All addresses are 7-bit. The esp_codec_dev macros are the 8-bit form (7-bit shifted left once). Pass 0x30/0x80 to audio_codec_i2c_cfg_t.addr, but 0x18/0x40 to the raw es8311/es7210 libraries and to i2cdetect.

- ES8311 (U6) = 0x18.
  - CE (pin 20, net Codec_CE) goes to AGND through R25 10k (codec crop re-read). The es8311.h comment gives 0x18 with CE low, 0x19 with CE high.
  - esp_codec_dev ES8311_CODEC_DEFAULT_ADDR = 0x30. Arduino ES8311_ADDRRES_0 = 0x18.
- ES7210 (U8) = 0x40.
  - AD0/AD1 go to AGND through R74/R42 0R; the 3V3 pads R36/R35 are NC. The schematic prints "0x40".
  - esp_codec_dev ES7210_CODEC_DEFAULT_ADDR = 0x80. Arduino ES7210_I2C_ADDR = 0x40.
- TCA9555PWR = 0x20.
  - A0/A1/A2 go to GND through the 3-pad jumpers R33/R18/R22 (0R on the GND leg, NC on the 3V3 leg).
  - IDF/xiaozhi use ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000; Arduino uses TCA9555_ADDRESS 0x20.
- PCF85063ATL RTC (U5) = 0x51, fixed by the part (Arduino PCF85063_ADDRESS 0x51).
  - Its INT (pin 4) goes to TCA EXIO4, not to a GPIO.
  - VDD comes from 3V3 through the D2 B5819WS diode, with the J9 backup cell directly on VDD.

Nothing else is on the bus on-board. There is no IMU, and the charger (ETA6098) and 3V3 buck (MP1605) are not on I2C.

Only with accessories plugged in:
- LCD touch controller (depends on the panel; from the demo drivers): FT6336 0x38, CST816 0x15, AXS5106 0x63, CST328 0x1A. TP_RST is on EXIO1 and TP_INT on EXIO2.
- Camera SCCB on the same bus: OV2640 0x30, OV5640 0x3C (the standard sensor addresses). xiaozhi runs it at 100 kHz.

## tca9555

TCA9555PWR at 0x20. "EXIOn" in Waveshare's pin table, wiki and code is 0-based and means bit n:
- EXIO0-7 = P00-P07 (port 0: input reg 0x00, output reg 0x02, config reg 0x06).
- EXIO8-15 = P10-P17 (port 1: 0x01, 0x03, 0x07).
- In code: IDF IO_EXPANDER_PIN_NUM_n = 1<<n; Arduino TCA9555_EXIOn = n.

The map below was re-verified from the schematic pin-table text coordinates (Extend_IOn rows at y = 322.6 ... 388.5 in bbox.html) and the TCA symbol, and cross-checked against the factory, lvgl, Arduino and xiaozhi code.
- EXIO0 / P00 = LCD_RST. Output; demos pulse it low then high.
- EXIO1 / P01 = TP_RST (touch reset). Output.
- EXIO2 / P02 = TP_INT (touch interrupt). Input.
- EXIO3 / P03 = SD_D3 (SD CS/DAT3), 10k pull-up R65. Leave it as an input so it reads high; the Arduino demo drives it HIGH before mounting.
- EXIO4 / P04 = RTC_INT, from PCF85063 INT (open-drain, active low). Input.
- EXIO5 / P05 = CAM_PWDN. LOW = camera on (Camera_EN writes false/LOW), HIGH = powered down (Camera_DIS).
- EXIO6 / P06 (pin 10) = Camera_SEL, through R51 1k to the FSUSB42UMX SEL pin.
  - HIGH: camera PCLK/XCLK use GPIO44/43, UART0 is lost, USB keeps working. This is the power-on default via the pull-up.
  - LOW: camera uses GPIO19/20, USB is lost, and every flash needs manual BOOT+RESET.
- EXIO7 / P07 = nothing (empty table row, y = 353.5).
- EXIO8 / P10 (pin 13) = PA_CTRL, the speaker amp enable: R30 0R to NS4150B CTRL, R31 10k pull-down to AGND.
  - HIGH = amp ON, LOW = shutdown.
  - Confirmed by the factory Audio_PA_EN (Set_EXIO(PIN_NUM_8, true)), the Arduino Audio_PA_EN, xiaozhi (set_level(PIN_NUM_8, 1) "enable speaker amp") and the NS4150B datasheet (CTRL VIH at least 1.2 V = on, VIL at most 0.2 V = shutdown).
- EXIO9 / 10 / 11 (P11-P13) = Key1 / Key2 / Key3. Inputs, active low, 10k pull-ups.
- EXIO12-15 (P14-P17) = free; they appear only in the table's extension column.

There is no other mux pin, no reset pin (pin 1 is INT#), and the backlight is on GPIO5.

INT# (pin 1) goes through R15, a 3-pad jumper whose legs are 3V3 and GPIO0.
- Re-read on the tca/r15 crops using the label-to-leg rule calibrated on R18, R22 and R33. On all three the "0R" half sits beside the GND leg, which gives address 0x20, matching the code.
- R15's body reads "NC 0R": NC beside pin 3 (GPIO0) and 0R beside pin 2 (3V3). So INT# is tied to 3V3 and NOT routed to GPIO0.
- Consequences:
  - Key presses and RTC alarms cannot interrupt or wake the ESP32.
  - Poll input register 0x01 (keys) and 0x00 (RTC_INT) over I2C.
  - Reading the inputs also clears INT#. That matters because an open-drain INT# held low against a hard 3V3 tie exceeds its 6 mA IOL rating. So read both input ports routinely (e.g. every poll).

Power-on state (TI datasheet):
- All pins are inputs: config 0x06/0x07 = 0xFF, output latches 0x02/0x03 = 0xFF, with internal pull-ups (about 100 kohm) to VCC.
- So EXIO6 powers up high (USB safe) and EXIO5 high (camera off).
- PA_CTRL sits at about 3.3 x 10k/110k = 0.3 V: far below VIH (1.2 V) but above the datasheet's VIL max (0.2 V). In practice the amp is off, but that is not guaranteed by spec.

Driver facts (esp_io_expander + tca95xx_16bit sources, read):
- new_i2c_tca95xx_16bit() runs reset(), which writes direction = 0xFFFF and output = 0xFFFF.
- set_level() refuses pins that are still inputs (ESP_ERR_INVALID_STATE).
- Therefore set_dir(PIN_8, OUTPUT) drives the amp HIGH immediately.
- The driver CACHES the output and direction registers (read_output_reg returns tca->regs.output). A raw I2C write behind its back desyncs the cache, and the next set_level() on any pin writes the stale 0xFFxx back, turning the amp on again. See boot_safety for the glitch-free, cache-coherent sequence.

How the demos set directions:
- Factory: outputs 0, 1, 5, 6, 8; inputs 2, 9, 10, 11.
- xiaozhi: outputs 0, 1, 5, 6, 8. It pulses LCD/touch reset, sets PA = 1 permanently, CAM_PWDN = 0 and Camera_SEL = 1.
- Arduino: TCA9555PWR_Init(0x0000) makes ALL pins outputs, keys included. Do not copy that.

Noise-monitor recommendation: make only EXIO8 an output, held low except around chimes. Leave everything else as inputs.

## mics

Two capsules, MIC1 and MIC2. The schematic draws each as a 4-pad part (VDD, GND, GND, DAT); the STEP model footprint is "MIC-4X3X1MM" (4.0 x 3.0 x 1.0 mm).

They are ANALOG MEMS, although the wiki and product page say "dual digital microphone array". Evidence from the mic1/adcA/adcB crops:
- VDD comes from ES7210 MICBIAS12 (net ADC_MICBIAS12, C101 1uF; local decoupling C71 100n + C33 2.2u). esp_codec_dev sets REG41/42 = 0x70, i.e. 2.87 V; Arduino uses ES7210_MIC_BIAS_2V87.
- DAT is AC-coupled into the ES7210 analog inputs.
- ES7210 DMIC_CLK (pin 14) and INT (pin 13) go only to test pads.

Signal path, pseudo-differential:
- MIC1: DAT -> L7 0R -> C32 1uF -> node MIC1_P (C86/C87 NC) -> C100 1uF -> MIC1P (pin 16). MIC1N (pin 15) goes through C99 1uF, then C85 1uF, to AGND.
- MIC2: the same path through L6 / C53 / C104 -> MIC2P (pin 19), and C107 / C66 -> MIC2N (pin 20).
- The C72/C73 and C59/C60 filter caps are NC.

Coupling high-pass (new; corrects B's 26 Hz). There are TWO 1 uF caps in series on each P input (0.5 uF effective).
- The ES7210 input impedance is 6 kohm at PGA 15 dB and above, 24 kohm at 12 dB and below (datasheet).
- So the corner is about 53 Hz at PGA of 15 dB or more (up to about 106 Hz if the 6k is differential, because the N side also has 0.5 uF). At PGA of 12 dB or less it is about 13-27 Hz.
- For A-weighting, low-frequency-dominated noise (traffic, HVAC) reads slightly low at high PGA. Either run PGA at 12 dB or less, or compensate in DSP. Measure it with a tone sweep.
- esp_codec_dev also enables the ES7210 digital HPF (regs 0x20-0x23 = 0x0a/0x2a).

ES7210 channels:
- CH1 = MIC1 and CH2 = MIC2 are the two real mics.
- CH3 (pins 31/32) = speaker loopback, the AEC reference. It is ES8311 OUTP/OUTN through an RC attenuator (R37/R75 2.2k, C75/C82 0.47u, R38/R45 10k, C81 2.2n, R39/R46 20k, R40 4.3k shunt, C76/C83 0.22u), about -24 dB.
- CH4 (pins 27/28) is unused: C93/C94 1uF to AGND. MICBIAS34 powers nothing.

TDM slot order: CH1, CH3, CH2, CH4.
- Source: ES7210 datasheet Fig. 2e: LRCK low carries CH1, CH3; LRCK high carries CH2, CH4.
- esp_codec_dev enables TDM (REG12 = 0x02) when 3 or more mics are selected.
- Reading layouts:
  - xiaozhi (4-slot TDM read): slots 0-3 = MIC1, MIC3 (ref), MIC2, MIC4.
  - Factory (2 x 32-bit standard read): int16 order [MIC3 ref, MIC1, MIC4, MIC2] = "RMNM". KMX confirmed this on hardware: "reference, microphone 1, unused, microphone 2".
- Gain masks use PHYSICAL mic numbering (es7210.c _es7210_set_channel_gain): bit0 = MIC1 (REG43), bit1 = MIC2, bit2 = MIC3, bit3 = MIC4.
- Alternative: select only MIC1|MIC2. That gives plain stereo I2S (left = MIC1, right = MIC2) on SDOUT1, but you lose the CH3 reference that marks chime playback.

Gain used by the demos: 30 dB PGA everywhere.
- Factory: RECORD_VOLUME 30.0 per channel.
- Arduino: ES7210_MIC_GAIN_30DB, ADC volume 0.
- xiaozhi: input_gain 30.0f.
- esp_codec_dev defaults to 30 dB in es7210_open.
- Community builds use 24 dB (ESPHome mic_gain 24db; KMX set_in_gain 24.0).
- Available steps: 0-33 dB in 3 dB steps, then 34.5, 36, 37.5.

NOT published anywhere examined: mic part number, sensitivity (dBV or dBFS at 94 dB SPL), SNR and AOP. Checked: schematic, wiki, product page, docs.waveshare.com and its Resources page, the STEP model (package only), all demos, xiaozhi, ESPHome and KMX. "MIC_MSM.cpp" in the Arduino demo is a carried-over filename, not a part reference.

Headroom maths (ES7210 datasheet: full scale = 2*AVDD/3.3 Vrms differential = 2.0 Vrms = +6.0 dBV at AVDD 3.3 V; SNR 102 dB(A) typ, measured at 48 kHz, MCLK/LRCK = 256):
- dBFS at 94 dB SPL = S + G - 6.0, where S = mic sensitivity (dBV/Pa) and G = PGA gain.
- Sine clip SPL = 100 - G - S.
- Example with S = -38 dBV/Pa (placeholder only): G = 30 clips at about 108 dB SPL, G = 24 at about 114, G = 12 at about 126 (probably above the mic's AOP).
- The quiet-room floor will be set by mic self-noise (unknown; expect roughly 30 dBA).

Calibration is mandatory: a 94 dB / 1 kHz calibrator, or a side-by-side reference meter, per unit and at the chosen gain. For a noise monitor, pick G of 24 dB or less (12 dB gives the flattest low end), measure clipping, and measure the floor.

Passband is 0.4535 fs: about 7.3 kHz at 16k, 10.9 kHz at 24k, 21.8 kHz at 48k.

## amp

Amplifier: U7 NS4150B (Nsiway), a filterless mono class-D amp, verified on the schematic.

Wiring:
- CTRL (pin 1) comes from PA_CTRL = TCA EXIO8 through R30 0R, with R31 10k to AGND. HIGH = on.
- Bypass (pin 2): C61 1uF.
- Inputs come from ES8311 OUTP/OUTN through C40/C45 0.1uF and R62/R28 150k (Rin) to IN+/IN-, with C41/C47 22pF to AGND. The ES8311 outputs are themselves AC-coupled (C44/C46 1uF).
- Outputs PA_OUTL+/- go through L8/L5 (0R) to a GH1.25 2-pin speaker header. C39/C42/C43 are NC.
- VDD (pin 6) = VCC, the system rail, NOT 3V3:
  - On USB: USB_5V through the MBR230LSFT1G Schottky, about 4.6-4.7 V.
  - On battery: BAT_OUT through the DMP2066LSN P-FETs Q2/Q1 (SW1 on, USB absent), 3.0-4.2 V.
  - Decoupling: C55 10u, C56 100n, C57 1u.

Datasheet figures (Nsiway NS4150B V1.1, Mar 2021, /private/tmp/speaker/ns4150b.txt; this is the correct part):
- VDD 3.0-5.0 V operating (4.8 V typ); absolute max 5.25 V.
- Gain AVD = 240k/Rin = 240k/150k = 1.6 V/V, about +4.1 dB.
- Input high-pass = 1/(2*pi*150k*0.1uF), about 10.6 Hz.
- Output at 5 V: 2.0 W / 4 ohm and 1.3 W / 8 ohm at 1% THD; 2.8 W / 1.7 W at 10% THD. Less on battery.
- CTRL VIH at least 1.2 V, VIL at most 0.2 V.
- Start-up time Tst = 120 ms typical; shutdown time Tsd = 80 ms; shutdown current 0.1 uA typ / 10 uA max.
- It has built-in pop suppression and over-current, over-temperature and under-voltage protection.
- B's 30 ms start-up, 5.25 V operating limit and "no gain formula" came from the non-B NS4150 sheet.

Speaker:
- The board STEP model contains "SPK-4020-5W", a 40 x 20 mm cavity speaker.
- Impedance and power are not published. Read the label on the owner's speaker.

Max safe volume: no manufacturer figure exists.
- esp_codec_dev 0-100 volume: factory default 60, KMX 40, ESPHome clamps 0.4-0.8 ("the onboard amp distorts near the top"), Arduino demo defaults to 98.
- esp_codec_dev hw_gain: its default (pa_voltage 5.0, dac 3.3, pa_gain 0, in esp_codec_dev_vol.c) equals xiaozhi's explicit values, so both apply -3.6 dB. Neither enters the NS4150B's real +4.1 dB as pa_gain.
- Recommendation:
  - Chime at volume 60 or lower; hard cap 80.
  - Chime file peaking at -6 dBFS or lower.
  - Listen for clipping, and re-test on battery, where the lower rail clips sooner.
  - Keep chimes short: the mics sit centimetres from the speaker.

## codec_init

All three working recipes use the ESP32 as I2S master and both codecs as slaves. The ESP-IDF recipes use espressif/esp_codec_dev: factory lock 1.5.1 on IDF 5.5.0; xiaozhi about 1.6.2. The wiki FAQ says to use IDF 5.4.1 if the demo reset-loops.

(A) Waveshare factory_01 / esp_sr_02 (hardeware_driver/bsp_board.c):
1. I2C master: port 0, SDA 11, SCL 10.
2. I2S: I2S_NUM_1, master, full duplex from one i2s_new_channel(&tx, &rx) call.
   - Both channels in std mode with I2S_STD_CLK_DEFAULT_CONFIG(16000), i.e. MCLK 256 x fs = 4.096 MHz.
   - Slots: I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, STEREO).
   - Pins: mclk 12, bclk 13, ws 14, dout 16, din 15.
   - Both channels are enabled BEFORE the codecs are opened.
   - The I2S_CONFIG_DEFAULT macro hardcodes 16000 and ignores its argument.
3. ES7210:
   - audio_codec_new_i2c_ctrl(addr 0x80).
   - es7210_codec_new(mic_selected = MIC1|MIC2|MIC3|MIC4; master_mode defaults to false, i.e. slave). Selecting 3 or more mics turns TDM on.
   - esp_codec_dev_open(in, {16000, channel 2, bits 32}). In TDM with 2 or fewer channels and no mask, the driver halves bits to 16, so each frame is 4 x int16 = [MIC3 ref, MIC1, MIC4, MIC2].
   - esp_codec_dev_set_in_channel_gain(MASK(0..3), 30.0).
   - es7210_open register writes: reset 0xFF then 0x41, HPF 0x2a/0x0a/0x0a/0x2a, slave, REG40 = 0x43, bias 0x70/0x70, OSR 0x20, REG02 = 0xC1, mic power/select, 30 dB.
4. ES8311:
   - Address 0x30, codec_mode DAC, pa_pin -1, use_mclk = false.
   - set_out_vol(60), then open({16000, 2 ch, 32 bit}).
   - The amp is switched separately on EXIO8.

(B) xiaozhi BoxAudioCodec, which the owner's unit runs (box_audio_codec.cc):
- 24 kHz for both directions (it asserts input rate == output rate). I2S_NUM_0, master; DMA 6 descriptors x 240 frames.
- TX: std mode, 16-bit stereo Philips, mclk_multiple 256 (6.144 MHz), din unused.
- RX: TDM, 16-bit, slots 0-3, mclk_multiple 256, bclk_div 8, total_slot auto, dout unused.
- ES8311: use_mclk = true, hw_gain 5.0/3.3, opened as 16-bit, 1 channel.
- ES7210: all 4 mics, opened as 16-bit, 4 channels, channel_mask MASK(0) | MASK(1) = MIC1 + ref. Gain 30 dB on MASK(0).
- Board code sets EXIO8 = 1 (amp on for good) and EXIO6 = 1.

(C) Arduino:
- es7210 library: address 0x40, 16 kHz, I2S format, mclk_ratio 256, 16-bit, bias 2.87 V, 30 dB, tdm_enable = true, ADC volume 0.
- es8311: address 0x18, MCLK from the pin, 256 x fs, 16/16-bit.
- Then Audio_PA_EN, with a 50 ms delay after each EXIO write.

Sample-rate facts, settled from the driver source:
- ES7210 in slave mode NEVER consults its coefficient table. es7210.c es7210_config_sample returns OK before get_coeff when master_mode is false.
  - Its clocking is fixed at REG02 = 0xC1 and OSR 0x20, which is exactly the table's 256 x fs setting (4.096M/16k, 8.192M/32k and 12.288M/48k all resolve to 0xC1).
  - So any fs in the 8-48 kHz single-speed range works as long as MCLK = 256 x fs. That is why xiaozhi's 24 kHz works despite no 6.144M/24k ES7210 entry.
- ES8311 DOES consult its table. It has 4.096M/16k, 6.144M/24k and 12.288M/48k (es8311.c coeff_div).
- The ES7210 datasheet characterises the part at 48 kHz with MCLK/LRCK = 256.
- So 48 kHz with MCLK 12.288 MHz is supported by both codecs and gives A-weighting to about 21.8 kHz. It is untested on this board, so verify it with a test tone. 16 kHz tops out around 7.3 kHz.
- ES8311 use_mclk = false (factory, KMX) derives its internal clock from SCLK with a fixed x8 multiplier (es8311.c:445-452). That equals 256 x fs only when BCLK = 32 x fs (16-bit stereo slots). The factory pairs it with 32-bit slots, giving 512 x fs. It works, but prefer use_mclk = true with MCLK = 256 x fs on GPIO12.

Recommended reproduction for the noise monitor (factory shape with the fixes above):
1. Bring up I2C 0 (SDA 11, SCL 10).
2. Bring up the TCA9555 with the amp held low (see boot_safety).
3. Create the full-duplex I2S master channels (std Philips, 32-bit stereo slots, mclk_multiple 256, pins 12/13/14/16/15, fs = 16000 first, then 48000). Enable both channels so MCLK runs.
4. ES7210: 0x80, 4 mics (TDM), open {fs, 2, 32}.
   - Set gain per mic with MASK(0) and MASK(1), physical numbering; choose 12-24 dB for monitoring.
   - Read 4 x int16 per frame. MIC1 = [1], MIC2 = [3], ref = [0].
   - A-weight each mic, power-average the two, and use [0] to flag playback.
5. Read back ES7210 reg 0x00 == 0x41 and 0x02 == 0xC1. If they don't match, close and re-open once (cold-boot guard).
6. ES8311: 0x30, DAC mode, use_mclk = true, pa_pin -1.
   - Open {fs, 2, 32} and write mono int16 << 16 into both slots (as KMX does).
   - Volume 60 or lower.
7. The chime must be rendered at the capture fs, because TX and RX share BCLK/WS.

Change one variable at a time from a proven baseline: 16 kHz first, then 48 kHz, then the gain.

## boot_safety

1. Amp off first.
   - At power-on the expander pins are inputs. R31 10k against the roughly 100k internal pull-up leaves PA_CTRL at about 0.3 V: nominally off, but not guaranteed by spec (VIL max 0.2 V).
   - esp_io_expander's reset() then writes output = 0xFFFF, so a plain set_dir(PIN_8, OUTPUT) turns the amp ON.
   - Glitch-free AND cache-coherent sequence:
     a) create the handle with esp_io_expander_new_i2c_tca95xx_16bit (this runs reset: all inputs, latch 0xFFFF);
     b) call io->write_output_reg(io, 0xFFFF & ~IO_EXPANDER_PIN_NUM_8), a public member of esp_io_expander_t that updates both the chip and the driver cache;
     c) call esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_8, IO_EXPANDER_OUTPUT), which now drives LOW with no pulse.
   - Do NOT use raw I2C writes to 0x03/0x07 alongside the driver: the cache still holds 0xFFFF, and the next set_level on any pin turns the amp back on.
   - Alternatively, own the TCA9555 entirely with raw I2C: write 0x03 bit0 = 0, then 0x07 bit0 = 0.
   - Any residual few-millisecond glitch is harmless, given the NS4150B's 120 ms start-up.

2. Chime sequence.
   - Codec first, amp second (Waveshare's order). Stream zeros so the ES8311 VMID and the AC-coupled outputs settle.
   - Raise EXIO8 and wait at least 120 ms (NS4150B Tst; the demos' 10-50 ms delays would clip the chime's start). Use about 150 ms.
   - Play the chime at 60 volume or lower, then 50 ms or more of silence, then EXIO8 LOW (Tsd 80 ms).
   - Gate or flag dBA from amp-on until about 100 ms after amp-off. The mics hear the speaker, and CH3 (ref) shows exactly when it is playing.
   - An enabled amp with an idle DAC hisses, which is another reason to keep it off between chimes.

3. Keep USB alive.
   - Never drive EXIO6 (Camera_SEL) low: that moves GPIO19/20 to the camera and forces manual BOOT+RESET for every flash.
   - Leave EXIO5, 6 and 7 as inputs. The pull-ups give camera powered down, mux on UART0 and USB intact.

4. SD. EXIO3 (SD_D3) must read high when the card powers up (low selects SPI mode). Leave it as an input (R65 10k plus the internal pull-up). Drive it HIGH as an output only if mounting fails.

5. Keys. EXIO9-11 must stay inputs; the Arduino TCA9555PWR_Init(0x0000) makes them outputs, so pressing a key shorts an output to ground.

6. INT# and GPIO0.
   - Per the R15 reading, INT# is tied to 3V3, not GPIO0, so there is no strap risk.
   - Read input ports 0x00 and 0x01 regularly (keys, RTC_INT) to clear INT#.
   - Belt and braces: read them at boot and before any esp_restart or deep sleep.
   - Confirm once on hardware: press a key without reading the TCA, then check gpio_get_level(0) == 1.

7. Strapping pins.
   - GPIO0 = BOOT (10k pull-up).
   - GPIO45 = CAM_D4 (must be low: VDD_SPI) and GPIO46 = CAM_D5 (10k pull-down R76) must be low at reset.
   - GPIO3 = LCD_CS.
   - All fine with nothing in J3/J6. A camera that drives D4/D5 high can stop the board booting.

8. WS2812 on GPIO38.
   - It floats until RMT starts, so LEDs can latch random colours: init RMT and clear the strip first thing at boot.
   - Cap brightness (good for night mode too). The ring runs from the shared 3V3 MP1605 buck (2 A); 7 LEDs at full white draw a few hundred mA.

9. No power-hold pin.
   - SW1 is a mechanical battery switch; Q1 is gated by USB presence; USB feeds VCC through a Schottky. Nothing has to be driven to stay powered, and there is no soft-off.
   - The ETA6098 charger's LED1 is hardware-only.

10. RTC and night mode.
   - The PCF85063 VDD comes from 3V3 through the D2 B5819WS diode, and the J9 cell sits directly on VDD with no current limit. It floats at about 3.0 V.
   - The wiki says to use a RECHARGEABLE RTC battery. Never fit a primary cell (CR2032), and expect a 3.6 V LIR cell to sit only partly charged (the same LIR trap as the DS3231 note).
   - With no cell, time survives only while 3V3 is up (USB, or the main LiPo with SW1 on).
   - RTC_INT is on EXIO4 and INT# reaches no GPIO, so an RTC alarm cannot wake the ESP32. Use the ESP32 RTC timer for wake-ups, and read the PCF85063 (0x51) over I2C for wall-clock night mode.

11. Cold boot. One unanswered HA thread reports the ES7210 and LEDs not coming up after a cold boot. Its dump (TCA 0x06 = 0xFF, ES7210 0x00 = 0x32, 0x02 = 0x02) shows power-on defaults, i.e. init never landed.
   - Start MCLK before the codec open.
   - Read back ES7210 0x00 == 0x41 and 0x02 == 0xC1, and retry once.
   - Initialise the LEDs independently of the expander.

## confidence

HIGH. The schematic and several code bases agree, and I re-verified these against primary sources in this pass:
- Every GPIO: I2S 12/13/14/15/16 on one shared clock; I2C 11/10; LED 38; SD 40/42/41 plus EXIO3; BOOT 0; keys on EXIO9-11.
- The I2C addresses (0x18, 0x40, 0x20, 0x51).
- The whole EXIO map, by pin-table coordinates, including EXIO8 = PA_CTRL active HIGH, EXIO6 = Camera_SEL, EXIO5 LOW = camera on, and EXIO4 = RTC_INT.
- The LED chain order U9..U10 and the count of 7.
- The battery-ADC jumpers (default not connected).
- Key4 = BOOT and Key5 = RESET.
- The NS4150B part and figures (120 ms start-up, 1.6 V/V gain, VDD on VCC).
- The ES7210 channel use (MIC1/MIC2 real, CH3 loopback, CH4 unused) and the TDM order CH1, CH3, CH2, CH4 (datasheet, two code bases, KMX on hardware).
- The analog-mic wiring and MICBIAS 2.87 V.
- The init recipes, copied from the code.
- The driver behaviours: ES7210 slave mode skips the coefficient table; the TCA driver reset writes 0xFFFF, caches its registers and refuses set_level on inputs.

MEDIUM-HIGH:
- R15 (INT# tied to 3V3, not GPIO0). It rests on the jumper-label convention calibrated on R18, R22 and R33 against address 0x20; a one-time hardware check is recommended.
- Mics are analog. The circuit is unambiguous but contradicts the wiki's "digital".

MEDIUM:
- LED colour order RGB: three board-specific sources, two of them hardware-tested community builds, but not yet seen on this unit.
- 48 kHz operation: both codecs support it and the ES7210 is characterised there, but it has not been run on this board.
- The coupling-HPF estimate: about 53 Hz or more at PGA 15 dB and above, about 13-27 Hz at 12 dB and below. It depends on whether the ES7210's 6k/24k input impedance is per-pin or differential; measure it.
- Amp rail voltages, which are read off the power path.

LOW:
- The cold-boot ES7210 failure (a single unanswered forum post).

UNKNOWN:
- Mic part number, sensitivity, SNR and AOP. Calibration is mandatory.
- Speaker impedance and power (only the STEP name SPK-4020-5W).
- The physical position of LED index 0.
- A manufacturer maximum volume.

## disagreements

Each item gives the verdict and how it was settled.

1. NS4150B start-up time, gain formula and VDD (A: 120 ms, AVD = 240k/Rin, 3.0-5.0 V; B: 30 ms, no formula, 3.0-5.25 V operating). A is right.
   - The part on the board is U7 NS4150B (schematic). The NS4150B V1.1 datasheet (ns4150b.txt) gives: operating VDD 3.0/4.8/5.0 V; absolute max 5.25 V; Tst 120 ms; Tsd 80 ms; ISD 0.1/10 uA; section 9.9 AVD = 240k/Rin.
   - B's figures come from the non-B NS4150 sheet (code/ds/ns4150.txt, lines 137 and 258-259).
   - Consequence: wait at least 120 ms after EXIO8 goes high, not 30 ms.

2. TCA9555 INT# via R15 (A: tied to 3V3, not GPIO0; B: can't be read, treat as a GPIO0 strap risk). A's reading is upheld.
   - I re-read crops/tca.png and crops/r15.png. On R18, R22 and R33 the "0R" half of the label sits beside the GND leg, which gives address 0x20 as the code requires.
   - R15 reads "NC 0R" with NC beside pin 3 (GPIO0) and 0R beside pin 2 (3V3).
   - B's hardware check (press a key without reading the TCA, then read GPIO0) is kept as confirmation. Reading the input ports regularly is advised either way.

3. ES7210 sample-rate support (A: 48 kHz is in the coefficient table; B: there is no 6.144 MHz/24 kHz entry, so use 16 or 48 kHz). Both miss the mechanism.
   - es7210.c es7210_config_sample returns before get_coeff when master_mode is false, and every recipe runs the ES7210 as a slave.
   - Its clocking is the fixed REG02 = 0xC1, which equals the table's 256 x fs rows. So any 8-48 kHz rate with MCLK = 256 x fs works; that is how xiaozhi runs 24 kHz.
   - The ES8311 table is consulted and has 16k, 24k and 48k at 256 x fs.

4. ES8311 use_mclk (factory and KMX false; xiaozhi and Arduino true). Use true.
   - MCLK is wired to GPIO12 (codec crop).
   - With false, es8311.c:445-452 multiplies SCLK by 8. That equals 256 x fs only with 16-bit slots; the factory's 32-bit slots give 512 x fs.

5. Glitch-free amp preload. Both reports suggested raw writes to registers 0x03 then 0x07. That is safe only if you never use esp_io_expander afterwards.
   - The tca95xx driver caches the output register (read_output_reg returns tca->regs.output). The next set_level on any pin would rewrite 0xFFxx and turn the amp on.
   - Use the public io->write_output_reg(io, 0xFFFF & ~PIN_8), then set_dir.

6. Key4/Key5 (A: BOOT = Key4 on GPIO0, RESET = Key5; B: unconfirmed). A is right, per crops/keys45.png (IO0 with R27 10k; RESET with R34 10k and C25 1uF).

7. EXIO5 polarity (A: "sets it false, then true"; B: LOW = on). B's wording is right; A described the call order.
   - lvgl camera_driver.c:41-48 and Arduino Camera_Driver.cpp:9-16: Camera_EN writes LOW, Camera_DIS writes HIGH.

8. EXIO6 vs EXIO7 for the camera/USB mux. It is EXIO6.
   - bbox pin-table rows: Extend_IO6 at y = 349.0 is Camera_SEL; the Extend_IO7 row at y = 353.5 is empty.
   - The code (Set_EXIO(PIN_NUM_6)) and the wiki agree. The schematic's Chinese "EXIO7" note counts from 1.

9. Keys on EXIO9/10/11 and PA_CTRL on EXIO8, not 12. bbox: Key1/2/3 at y = 362.1/366.5/371.0 match the Extend_IO9/10/11 rows, and PA_CTRL at y = 357.8 matches Extend_IO8. The demo reads PIN_9/10/11 and drives PIN_8.

10. LED count (xiaozhi 6 vs 7). Use 7. Evidence: seven WS2812B-0807 on the schematic chain (verified U9 -> U19 -> U14 -> U13 -> U12 -> U11 -> U10), the wiki's "7x surround RGB LEDs", factory LED_STRIP_LED_COUNT 7, ESPHome and KMX. xiaozhi's CircularStrip(38, 6) is wrong for this board.

11. LED colour order (RGB vs GRB). Default to RGB.
   - The factory code sets LED_STRIP_COLOR_COMPONENT_FMT_RGB explicitly. Its trailing "GRB" comment appears to be left over from the stock led_strip example.
   - KMX README: "tested revision uses RGB order". ESPHome's working config: rgb_order RGB.
   - xiaozhi's FMT_GRB is a generic class shared by every board.
   - Still confirm with pixel 0 = (255, 0, 0) lighting red.

12. Mic coupling high-pass (B: 26 Hz). Corrected to about 53 Hz or more at PGA 15 dB and above, about 13-27 Hz at 12 dB and below.
   - crops/mic1.png and adcB.png show two 1 uF caps in series (C32, then C100) on each P input, i.e. 0.5 uF, with the N side AC-grounded through C99 + C85.
   - ES7210 input impedance is 6k/24k (datasheet). A did not quantify it.

13. Battery ADC. No real conflict: A and B agree. crops/batj.png confirms R53 (GPIO1 to CAM_HREF fitted), R78 (BAT_ADC to R53's empty pad) and the GPIO6 option. Arduino's BAT_ADC_PIN 8 is a copy-paste error (GPIO8 is LCD MISO).

14. Mic type. The wiki and product page say "digital"; the schematic shows analog 4-pad capsules on the ES7210 analog inputs with MICBIAS12, and DMIC_CLK goes only to a test pad. Analog.

15. Slot order "RMNM" vs CH3 = ref (the ESPHome notes list this as open). It is not a conflict: datasheet Fig. 2e order CH1, CH3, CH2, CH4, read as 2 x 32-bit little-endian slots, gives [CH3, CH1, CH4, CH2]. xiaozhi's comment and KMX's hardware test agree.

16. Mic gain (30 dB in factory, Arduino, xiaozhi and the esp_codec_dev default; 24 dB in ESPHome and KMX). Both work. For monitoring, choose 12-24 dB from measured headroom; 12 dB or less also lowers the coupling HPF.

17. Amp at boot (xiaozhi and factory leave it on; ESPHome keeps it off until playback). For this project: off except during chimes.

18. TCA directions. Arduino's TCA9555PWR_Init(0x0000) makes the key pins outputs, which is a bug. Keep EXIO2, 3, 4 and 9-11 as inputs.

19. LCD pins. factory_01 bsp_board.h (SCLK 5, MOSI 1, DC 3, CS 6) is stale. The lvgl example, xiaozhi and schematic give SCLK 4, MOSI 9, MISO 8, DC 7, CS 3, BL 5. Irrelevant with no screen.

20. RTC cell. The wiki says "rechargeable RTC battery". The charge path is only the D2 Schottky from 3V3 (about 3.0 V, no current limit). Use a rechargeable cell only, never a CR2032.

21. Cold-boot failure. Both reports mention it; B's register readback-and-retry is adopted. The thread's dump shows untouched power-on defaults, and "clocks first" alone is unproven.

22. Default volume (factory 60, KMX 40, ESPHome cap 0.8, Arduino 98). No manufacturer limit exists. Use 60 or lower for chimes, capped at 80.

STILL OPEN (no source has it): mic part, sensitivity, SNR and AOP; speaker impedance and power (only the STEP name SPK-4020-5W); the physical ring position of LED index 0. The following need a hardware check: LED colour order, the R15 fit, and 48 kHz capture.

## sources

- Schematic v1.1: https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf -> /private/tmp/speaker/ESP32-S3-AUDIO-Board_1.1.pdf (render /private/tmp/speaker/render/hi-1.png; text layer /private/tmp/speaker/sch.txt and /private/tmp/speaker/bbox.html, pin-table rows read by coordinate)
- Schematic crops re-read during reconciliation: /private/tmp/speaker/crops/{tca,r15,r33,keys,keys45,codec,mic1,adcA,adcB,pa1,pwrL,ledsL,ledsR,sd}.png and the new /private/tmp/speaker/crops/batj.png (R53/R78 battery-ADC jumpers)
- Demo zip: https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board-Demo.zip -> /private/tmp/speaker/demo/ESP32-S3-AUDIO-Board-Demo/
- /private/tmp/speaker/demo/ESP32-S3-AUDIO-Board-Demo/ESP-IDF/factory_01/main/hardeware_driver/bsp_board.{c,h} (I2S_NUM_1, 16k/2ch/32-bit, ES7210 4 mics slave, ES8311 use_mclk=false, RECORD_VOLUME 30, PLAYER_VOLUME 60, RMNM, SDMMC width 1)
- /private/tmp/speaker/demo/ESP32-S3-AUDIO-Board-Demo/ESP-IDF/factory_01/main/{rgb_led_driver/rgb_led_driver.c (GPIO38, 7 LEDs, FMT_RGB), tca9555_driver/, button_driver/, audio_play_driver/}
- /private/tmp/speaker/demo/ESP32-S3-AUDIO-Board-Demo/ESP-IDF/lvgl9_3_example_04/main/camera_driver/camera_driver.c (EXIO5 LOW = Camera_EN, EXIO6 HIGH = TX/RX for camera)
- /private/tmp/speaker/demo/ESP32-S3-AUDIO-Board-Demo/Arduino/examples/LVGL_Arduino/{Camera_Driver.cpp, MIC_MSM.*, Audio_ES8311.cpp, TCA9555PWR.*, RTC_PCF85063.h, BAT_Driver.*}
- xiaozhi board config: https://github.com/78/xiaozhi-esp32/tree/main/main/boards/waveshare/esp32-s3-audio-board -> /private/tmp/speaker/xiaozhi/{config.h, esp32-s3-audio_board.cc}
- xiaozhi codec: https://github.com/78/xiaozhi-esp32/blob/main/main/audio/codecs/box_audio_codec.cc -> /private/tmp/speaker/xiaozhi/box_audio_codec.{cc,h} (24 kHz, TX std 16-bit, RX TDM 4 slots, use_mclk=true, slot-order comment)
- xiaozhi LED: /private/tmp/speaker/xiaozhi/circular_strip.cc (generic CircularStrip, FMT_GRB) and esp32-s3-audio_board.cc:215 CircularStrip(38, 6)
- esp_codec_dev 1.6.2 / 1.5.1: /private/tmp/speaker/code/comp/esp_codec_dev_1.6.2/device/es7210/es7210.c (config_sample returns early in slave mode, lines 141-145; mic_select/TDM; gain masks), device/es8311/es8311.c (coeff_div incl. 6.144M/24k and 12.288M/48k; use_mclk=false x8 at lines 445-452), esp_codec_dev_vol.c (hw_gain default 5.0/3.3)
- esp_io_expander + tca95xx_16bit: /private/tmp/speaker/code/comp/ioexp/esp_io_expander.c and include/ (public write_output_reg member), /private/tmp/speaker/code/comp/tca95xx/esp_io_expander_tca95xx_16bit.c (reset writes 0xFFFF, output and direction cached)
- NS4150B datasheet V1.1 (correct part): https://xonstorage.z8.web.core.windows.net/pdf/nsiway_ns4150b_apr22_xonlink.pdf -> /private/tmp/speaker/ns4150b.pdf, ns4150b.txt (VDD 3.0-5.0 V, Tst 120 ms, Tsd 80 ms, AVD = 240k/Rin)
- NS4150 non-B datasheet (source of report B's 30 ms figure): https://aitendo3.sakura.ne.jp/aitendo_data/product_img/ic/power_amp/NS4150/NS4150.pdf -> /private/tmp/speaker/code/ds/ns4150.txt
- ES7210 datasheet rev 22: https://files.waveshare.com/wiki/common/ES7210-datasheet.pdf -> /private/tmp/speaker/code/ds/ES7210.txt (Fig 2e TDM order, full scale 2*AVDD/3.3 Vrms, input impedance 6k/24k, SNR 102 dB(A) at 48 kHz 256fs, 8-48 kHz single speed)
- TCA9555 datasheet: https://www.ti.com/lit/ds/symlink/tca9555.pdf -> /private/tmp/speaker/tca9555.pdf, tca9555.txt (POR all inputs, internal pull-ups, INT open-drain IOL 6 mA)
- Board 3D model: https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board.rar -> /private/tmp/speaker/code/drawing/ESP32-S3-AUDIO-Board.stp (MIC-4X3X1MM, SPK-4020-5W)
- Wiki: https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board -> /private/tmp/speaker/wiki.html, wiki.txt (pinout tables, 7x surround RGB LEDs, rechargeable RTC battery header, 'dual digital microphone array')
- https://www.waveshare.com/esp32-s3-audio-board.htm, https://docs.waveshare.com/ESP32-S3-AUDIO-Board and /Resources-And-Documents (no mic or speaker part listed)
- Community tier B: github.com/MichalZaniewicz/esphome-waveshare-esp32-s3-audio-va -> /private/tmp/speaker/code/esphome-va/{base/core.yaml (rgb_order RGB, 7 LEDs, mic_gain 24db, volume clamp 0.4-0.8), docs/HARDWARE.md (cold-boot thread dump, amp hiss)}
- Community tier B: github.com/KMX415/waveshare-ai-speaker-assistant -> /private/tmp/speaker/code/kmx/firmware/{README.md ('tested revision uses RGB order'; capture = reference, mic1, unused, mic2), main/board_audio.c (gain 24, volume 40, use_mclk=false)}
- Input reports A and B (reconciled here)