#include "envstate.h"

#include <string.h>

/*
 * Everything here is integers in the series' own fixed point, as the log is:
 * the display value, the state and the trend are compared against edges, and
 * an edge compared in float is an edge that moves by a rounding error.
 */

#define US_PER_MIN     (60LL * 1000000LL)

/* The display value is the median of this many valid readings: two minutes of
   the room at the 30 s cadence. */
#define MEDIAN_OF      4

/* The trend compares the last TREND_SPAN with the TREND_SPAN ending TREND_AGO
   ago, and says nothing until valid readings reach back TREND_NEED. */
#define TREND_SPAN     (10 * US_PER_MIN)
#define TREND_AGO      (30 * US_PER_MIN)
#define TREND_NEED     (TREND_AGO + TREND_SPAN)

/*
 * The limits: the ENS160 datasheet v1.3, Table 6 for TVOC and Table 5 for
 * eCO2, the edges where the chip's own table moves from "good" to "moderate"
 * and from there to "poor". The chip's own table because its ppb are
 * ethanol-equivalent: its TVOC cannot be held against another maker's limits,
 * only against the ones it was calibrated with. They replace the old
 * voc_thresh {300,1000} and co2_thresh {800,1200}, which came from nowhere in
 * particular and disagreed with each other.
 *
 * `top` and `floor` are the fixed chart scale, not states: 2200 ppb is the
 * datasheet's top TVOC band, and eCO2 is never below 400 (the chip's own
 * floor, the outdoor air it is derived against).
 */
static const envs_limits_t s_limits[] = {
    [ENVS_VOC]  = { .fair = 220, .poor = 650,  .top = 2200, .floor = 0 },
    [ENVS_ECO2] = { .fair = 800, .poor = 1000, .top = 1500, .floor = 400 },
};

/* Through int, because an enum with no negative member may be unsigned and a
   check for below zero on it is a warning that it can never be true. */
static bool series_ok(envs_series_t s)
{
    return (int)s >= 0 && (int)s < ENVS_N;
}

const envs_limits_t *envs_limits(envs_series_t s)
{
    return s == ENVS_VOC || s == ENVS_ECO2 ? &s_limits[s] : NULL;
}

/* The edge a state is left by falling back under: FAIR's own edge for FAIR,
   POOR's for POOR. */
static int32_t edge_of(const envs_limits_t *l, envs_state_t st)
{
    return st == ENVS_POOR ? l->poor : l->fair;
}

/*
 * Worse at once, better only 10 % below the edge. At once, because a room
 * that has just turned bad should say so now; slowly back, because a room
 * sitting at 219-221 ppb would otherwise flip GOOD, FAIR, GOOD every thirty
 * seconds, and a word that flickers is a word nobody believes. The number
 * beside it does not wait, so inside that 10 % the two can disagree ("210
 * FAIR" on the way down) -- deliberately.
 *
 * It steps down one state at a time and keeps stepping while it is under the
 * next edge's band too, so a window thrown open on a POOR room goes straight
 * to OK rather than stopping at a FAIR it has already fallen through.
 *
 * Temperature and humidity are shown raw with no word, so they are always OK:
 * there is nothing for them to be in.
 */
envs_state_t envs_classify(envs_series_t s, int32_t v, envs_state_t prev)
{
    const envs_limits_t *l = envs_limits(s);
    if (!l) return ENVS_OK;

    envs_state_t raw = v >= l->poor ? ENVS_POOR
                     : v >= l->fair ? ENVS_FAIR : ENVS_OK;
    /* OK has nothing below it to hold back from, and WAIT is no state at all:
       the first reading after a warm-up is judged as it stands. */
    if (prev != ENVS_FAIR && prev != ENVS_POOR) return raw;

    /* 10 v < 9 edge is v < 0.9 edge, exactly: 198 and 585, 720 and 900. */
    envs_state_t st = prev;
    while (st > raw && 10LL * v < 9LL * edge_of(l, st))
        st = st == ENVS_POOR ? ENVS_FAIR : ENVS_OK;
    return st > raw ? st : raw;
}

/*
 * Truncated to a step, so the last digit on screen is not sensor noise:
 * VOC in 5 ppb below 100, 10 below 1000 and 100 above; eCO2 in 10 ppm;
 * temperature to 0.1 C; humidity to 1 %.
 *
 * Truncated rather than rounded, so the number never shows an edge the reading
 * has not reached. Every edge is a multiple of its step, which makes "the
 * number is past 220" exactly "the reading is past 220": a GOOD 218 reads 210,
 * where rounding would put 220 beside the word GOOD. Toward zero, as C's
 * division is, which matters only for a temperature below freezing.
 */
static int32_t trunc_to(int32_t v, int32_t step)
{
    return v / step * step;
}

int32_t envs_quantise(envs_series_t s, int32_t v)
{
    switch (s) {
    case ENVS_VOC:  return trunc_to(v, v < 100 ? 5 : v < 1000 ? 10 : 100);
    case ENVS_ECO2: return trunc_to(v, 10);
    case ENVS_TEMP: return trunc_to(v, 10);         /* 0.01 C to 0.1 C */
    case ENVS_RH:   return trunc_to(v, 100);        /* 0.01 % to 1 % */
    default:        return v;
    }
}

/*
 * gas_start_us is "no gas reading yet" while negative. esp_timer counts up
 * from boot and is never negative, so -1 cannot be mistaken for a real time,
 * where zero -- a reading in the first microsecond -- in principle could.
 *
 * The gases start in WAIT, having said nothing yet; temperature and humidity
 * start and stay OK, having no word to wait for.
 */
void envs_init(envs_t *e)
{
    memset(e, 0, sizeof *e);
    e->gas_start_us = -1;
    e->state[ENVS_VOC] = ENVS_WAIT;
    e->state[ENVS_ECO2] = ENVS_WAIT;
    e->state[ENVS_TEMP] = ENVS_OK;
    e->state[ENVS_RH] = ENVS_OK;
}

/*
 * `head` is the next slot to write, so the newest reading sits just behind it
 * and the oldest, once the ring is full, is at it.
 *
 * The gas clock starts at the first reading that carries gas at all, valid or
 * not. That is as near to the chip's own power-on as this module can see --
 * up to one sample late -- and it is the right start for "12 MIN SO FAR",
 * which is counting the warm-up, and the warm-up is counted in readings the
 * chip flags as not yet good.
 */
void envs_push(envs_t *e, const envs_reading_t *r)
{
    if (e->gas_start_us < 0 && (r->have & ENV_HAVE_GAS))
        e->gas_start_us = r->t_us;
    e->r[e->head] = *r;
    e->head = (e->head + 1) % ENVS_RING;
    if (e->n < ENVS_RING) e->n++;
}

/* The i-th newest reading: 0 the latest, n-1 the oldest held. */
static const envs_reading_t *newest(const envs_t *e, int i)
{
    return &e->r[(e->head - 1 - i + 2 * ENVS_RING) % ENVS_RING];
}

/*
 * One series out of a reading, if the reading has it. A gas reading the chip
 * flags as anything but normal -- warming up, the first hour of a new part,
 * or invalid -- is a gap here, not a value: a warm-up spike must never set a
 * number, a word or a colour, and 0 ppb during warm-up is not clean air.
 */
static bool reading_value(const envs_reading_t *r, envs_series_t s, int32_t *out)
{
    bool gas = (r->have & ENV_HAVE_GAS) && r->validity == 0;
    switch (s) {
    case ENVS_VOC:
        if (!gas) return false;
        *out = r->tvoc;
        return true;
    case ENVS_ECO2:
        if (!gas) return false;
        *out = r->eco2;
        return true;
    case ENVS_TEMP:
        if (!(r->have & ENV_HAVE_TEMP)) return false;
        *out = r->temp_c100;
        return true;
    case ENVS_RH:
        if (!(r->have & ENV_HAVE_RH)) return false;
        *out = r->rh_c100;
        return true;
    default:
        return false;
    }
}

/*
 * The median of the last four valid readings, walking back past any gaps.
 * A median, so one spike -- a match struck, a door opening, a single bad
 * frame -- moves neither the number nor the word: it takes three readings in
 * a row. Fewer than four right after warm-up, down to one; with an even count
 * the mean of the middle two, truncated.
 *
 * No age limit: this module has no clock, and a sensor that has gone quiet is
 * noticed by the caller, which does. The ring itself is the outer bound --
 * nothing older than 48 minutes is left in it to be found.
 */
bool envs_value(const envs_t *e, envs_series_t s, int32_t *out)
{
    int32_t v[MEDIAN_OF];
    int k = 0;
    for (int i = 0; i < e->n && k < MEDIAN_OF; i++)
        if (reading_value(newest(e, i), s, &v[k])) k++;
    if (k == 0) return false;

    for (int i = 1; i < k; i++)                     /* four at most: insertion */
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int32_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
    *out = k & 1 ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
    return true;
}

/*
 * The smallest change that counts as moving: VOC max(15 ppb, 25 %) of where it
 * was, eCO2 50 ppm, temperature 0.5 C, humidity 3 %. Below these the sensor's
 * own wander would draw an arrow on a still room. VOC is relative as well
 * because its noise grows with it: 15 ppb matters at 40 and is nothing at 600.
 */
static int32_t min_move(envs_series_t s)
{
    switch (s) {
    case ENVS_VOC:  return 15;
    case ENVS_ECO2: return 50;
    case ENVS_TEMP: return 50;                      /* 0.01 C */
    case ENVS_RH:   return 300;                     /* 0.01 % */
    default:        return 0;
    }
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

/*
 * The mean of the last ten minutes against the mean of the ten minutes ending
 * thirty minutes ago: a span a person notices, and means rather than the
 * display value so one reading cannot turn the arrow.
 *
 * Unknown until the valid readings reach back forty minutes, so the older
 * window is real data and not whatever warm-up left. Forty minutes of time,
 * not eighty readings: a hygrometer that fails one CRC in five still has its
 * forty minutes and should still have its arrow. Unknown too when either
 * window holds nothing valid -- a gap is not a steady room.
 *
 * Compared cross-multiplied rather than divided: the difference of the means
 * is (Sr/nr - So/no), and times nr*no that is exact in integers, so an edge
 * case -- exactly 15 ppb, exactly 25 % -- lands on the side the rule says.
 */
envs_trend_t envs_trend(const envs_t *e, envs_series_t s, int64_t now_us)
{
    if (!series_ok(s)) return ENVS_UNKNOWN;

    int64_t recent_sum = 0, old_sum = 0;
    int recent_n = 0, old_n = 0;
    bool reaches_back = false;
    for (int i = 0; i < e->n; i++) {
        const envs_reading_t *r = newest(e, i);
        int32_t v;
        if (r->t_us > now_us || !reading_value(r, s, &v)) continue;
        int64_t age = now_us - r->t_us;
        if (age < TREND_SPAN) {
            recent_sum += v; recent_n++;
        } else if (age >= TREND_AGO && age < TREND_AGO + TREND_SPAN) {
            old_sum += v; old_n++;
        }
        if (age >= TREND_NEED) reaches_back = true;
    }
    if (!reaches_back || recent_n == 0 || old_n == 0) return ENVS_UNKNOWN;

    int64_t diff = recent_sum * old_n - old_sum * recent_n;    /* change x nr x no */
    int64_t scale = (int64_t)recent_n * old_n;
    bool moving = abs64(diff) >= min_move(s) * scale;
    /* 25 % of the old mean, So/no, is So x nr / 4 at this scale. */
    if (s == ENVS_VOC && 4 * abs64(diff) < abs64(old_sum * recent_n))
        moving = false;
    if (!moving) return ENVS_STEADY;
    return diff > 0 ? ENVS_RISING : ENVS_FALLING;
}

/*
 * The state is kept here rather than by the caller, so the hysteresis is the
 * ring's: whoever asks, and however often, the word is the same. A gas with
 * no valid reading in the ring waits, and forgets the state it had -- the
 * reading after a warm-up is judged afresh, not held to a FAIR from before.
 */
envs_state_t envs_update(envs_t *e, envs_series_t s)
{
    if (!series_ok(s)) return ENVS_WAIT;
    int32_t v;
    envs_state_t st;
    if (!envs_limits(s))                    st = ENVS_OK;
    else if (!envs_value(e, s, &v))         st = ENVS_WAIT;
    else                                    st = envs_classify(s, v, e->state[s]);
    e->state[s] = st;
    return st;
}

/*
 * Whether the gas pages should say WARMING UP (validity 1 or 2) or GAS ERROR
 * (3) in place of a number: the chip's own word on its newest gas reading.
 * The newest reading that carries gas, not the newest reading -- a room-only
 * reading says nothing about the gas.
 *
 * The minutes are "SO FAR", counted from the first gas reading, and never a
 * time remaining: the ENS160's initial start-up runs until it has had 24
 * hours unbroken power, resumes from the start if it loses power before then,
 * and the chip cannot say how far along it is.
 *
 * No gas reading at all is not warming up: it is a chip that has not
 * answered, and a page that said WARMING UP 0 MIN for ever about a dead
 * sensor would be a lie. The caller shows it as no data instead.
 */
bool envs_gas_warming(const envs_t *e, int *minutes_so_far, bool *error, int64_t now_us)
{
    if (minutes_so_far) *minutes_so_far = 0;
    if (error) *error = false;
    for (int i = 0; i < e->n; i++) {
        const envs_reading_t *r = newest(e, i);
        if (!(r->have & ENV_HAVE_GAS)) continue;
        if (r->validity == 0) return false;
        if (error) *error = r->validity >= 3;
        if (minutes_so_far && e->gas_start_us >= 0 && now_us > e->gas_start_us)
            *minutes_so_far = (int)((now_us - e->gas_start_us) / US_PER_MIN);
        return true;
    }
    return false;
}

/*
 * One line for the clock page: the worse of the two gases, naming the reading
 * at fault, VOC when they tie because VOC is the one Reza watches.
 *
 * eCO2 is not measured: the chip derives it from the same VOC signal (plus
 * hydrogen), so it is allowed to make the verdict worse and never better. Hence
 * the asymmetry at WAIT: no VOC state is no verdict, whatever eCO2 says, but
 * an eCO2 with no state cannot hide a POOR VOC.
 */
envs_verdict_t envs_verdict(const envs_t *e)
{
    envs_verdict_t v = { .worst = ENVS_VOC, .state = ENVS_WAIT };
    if (envs_gas_warming(e, NULL, NULL, 0)) return v;

    envs_state_t voc = e->state[ENVS_VOC], co2 = e->state[ENVS_ECO2];
    if (voc == ENVS_WAIT) return v;
    v.state = voc;
    if (co2 != ENVS_WAIT && co2 > voc) {
        v.worst = ENVS_ECO2;
        v.state = co2;
    }
    return v;
}

/* To the nearest, half away from zero. The stored record is a measurement,
   not a display: truncating it would bias a month of the log low. */
static int32_t mean_of(int64_t sum, int n)
{
    return (int32_t)(sum >= 0 ? (sum + n / 2) / n : -((-sum + n / 2) / n));
}

/*
 * The 5-minute flash record, from the 30 s readings in [from_us, to_us).
 *
 * A mean of the valid readings, where it used to be one reading: one sample
 * of noise standing for ten. The peak TVOC goes in hpa_x10 because envo has no
 * barometer and every reader asks ENV_HAVE_HPA before it touches that field;
 * ENV_MEANPEAK says so, and tells the new records from the older single
 * readings, whose peak is their only value.
 *
 * The room and the gas are separate: a warm-up makes the gas a gap but the
 * room was still measured. With no valid gas in the window, the record keeps
 * the chip's latest validity and no gas bit -- a gap that remembers why, so an
 * hour of settling is not later mistaken for an hour of bad air. aqi is the
 * latest valid reading's: a 1..5 index averaged is a number the chip never
 * said.
 *
 * `minute` is the caller's and is left as it finds it, whether it was set
 * before the call or is set after. The window must lie within the ring's 48
 * minutes; anything older has already gone.
 */
bool envs_fold5(const envs_t *e, int64_t from_us, int64_t to_us, env_sample_t *out)
{
    uint32_t minute = out->minute;
    memset(out, 0, sizeof *out);
    out->minute = minute;

    int64_t temp_sum = 0, rh_sum = 0, voc_sum = 0, co2_sum = 0;
    int temp_n = 0, rh_n = 0, gas_n = 0;
    bool any_gas = false;
    uint8_t last_validity = 0, aqi = 0;
    uint16_t voc_max = 0;
    for (int i = e->n - 1; i >= 0; i--) {           /* oldest first */
        const envs_reading_t *r = newest(e, i);
        if (r->t_us < from_us || r->t_us >= to_us) continue;
        if (r->have & ENV_HAVE_TEMP) { temp_sum += r->temp_c100; temp_n++; }
        if (r->have & ENV_HAVE_RH)   { rh_sum += r->rh_c100; rh_n++; }
        if (!(r->have & ENV_HAVE_GAS)) continue;
        any_gas = true;
        last_validity = r->validity;
        if (r->validity != 0) continue;
        voc_sum += r->tvoc;
        co2_sum += r->eco2;
        if (r->tvoc > voc_max) voc_max = r->tvoc;
        aqi = r->aqi;
        gas_n++;
    }

    out->flags = ENV_MEANPEAK;
    if (temp_n) {
        out->temp_c100 = (int16_t)mean_of(temp_sum, temp_n);
        out->flags |= ENV_HAVE_TEMP;
    }
    if (rh_n) {
        out->rh_c100 = (uint16_t)mean_of(rh_sum, rh_n);
        out->flags |= ENV_HAVE_RH;
    }
    if (gas_n) {
        out->tvoc_ppb = (uint16_t)mean_of(voc_sum, gas_n);
        out->eco2_ppm = (uint16_t)mean_of(co2_sum, gas_n);
        out->hpa_x10 = voc_max;
        out->aqi = aqi;
        out->flags |= ENV_HAVE_GAS | ENV_GAS_FLAGS(0);
    } else if (any_gas) {
        out->flags |= ENV_GAS_FLAGS(last_validity);
    }
    return temp_n || rh_n || any_gas;
}
