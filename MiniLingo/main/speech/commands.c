// MultiNet7-EN + die lokalen Commands (Spec Section 7 plus
// Volume-Stufen aus Phase 8b).
// Das Model liegt seit Phase 2 in der model-Partition; hier wird es
// geladen und vom afe_fetch_task parallel zum Recording gefuettert
// (Spec "Audio-Fetch-Task"-Geruest).
//
// Phonem-Besonderheiten (Diagnose 2026-06-11, siehe PROGRESS.md):
// - Das G2P-Token "MgNcTS" ("minutes") ist im FST von mn7_en defekt —
//   Phrasen damit erreichen NIE den Detected-State. Workaround:
//   Phonem-Registrierung ohne das Schluss-S ("MgNcT"); das gesprochene
//   Plural-S toleriert der Decoder als Junk-Suffix (gleicher Effekt,
//   durch den "stop timer" frueh als "stop" matcht).
// - Die On-Device-G2P uebersetzt "minute" als "MgNoT" ("min-oot");
//   gesprochen wird aber "MgNcT" ("min-it") — die Kurz-Aliasse
//   ("five minute timer" etc.) werden deshalb ebenfalls mit
//   expliziten Phonemen registriert.
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#include "commands.h"

static const char *TAG = "commands";

// MultiNet-Timeout: so lange darf nach dem Wake Word gesprochen werden,
// bis MultiNet aufgibt (laengere Aeusserungen sind Cloud-Queries).
#define MN_TIMEOUT_MS 6000

// Die Timer-Phrasen liegen acoustisch nah beieinander und matchen mit
// niedrigen Wahrscheinlichkeiten (0.05-0.15); der Default-Threshold
// laesst sie durchfallen. Phase 8b: von 0.05 auf 0.03 gesenkt, weil
// "volume sixty percent" bei 0.05 nur ~50% der Versuche schaffte
// (Matches bei 0.06-0.08, Rest knapp drunter — die "twenty"-Hypothese
// konkurriert im Decoder). Gegen False-Positives schuetzen die
// per-Command-Mindest-Probs (min_prob, Kurz-Commands 0.25) und dass
// MultiNet nur im Wake-Word-Fenster gefuettert wird; lange Phrasen
// brauchen ohnehin den kompletten Phonem-Pfad. Bei 0.01 gab es in
// Session 6 False-Positives — nicht weiter senken.
#define MN_DET_THRESHOLD 0.03f

// Per-Command-Mindestwahrscheinlichkeit oberhalb des globalen
// Thresholds (User-Report 2026-06-12): die kurzen Commands matchen
// auf deutsche freie Saetze, bevor der Satz zu Ende ist ("Wie..." ->
// "mute" / "volume up", beobachtete False-Positive-Probs 0.05-0.20).
// Ein verworfener Match beendet die Session NICHT — MultiNet lauscht
// weiter, und der Satz geht nach dem Timeout normal in die Cloud.
// Tuning-Punkt: Probs echter Kurz-Commands im Log pruefen (Match- und
// Reject-Zeilen loggen sie) und ggf. nachziehen.
static float min_prob(command_id_t id)
{
    switch (id) {
    case CMD_STOP:
    case CMD_MUTE:
    case CMD_UNMUTE:
    case CMD_VOLUME_UP:
    case CMD_VOLUME_DOWN:
        return 0.25f;
    case CMD_CHARACTER_ALEXA:
        // DIAGNOSE: 0 -> jeder Alexa-Treffer wird geloggt (Prob sichtbar),
        // um die beste Aussprache-Variante zu finden. Danach wieder anheben.
        return 0.0f;
    case CMD_CHARACTER_MYCROFT:
    case CMD_CHARACTER_JARVIS:
    case CMD_CHARACTER_NEXT:
        // Die drei "switch to X" teilen sich den Prefix; MultiNet raet den
        // Namen dann mit sehr niedriger Prob (~0.04) und matcht faelschlich
        // den ersten Charakter. 0.15 verwirft solche Low-Confidence-Treffer
        // (echte liegen bei 0.25-0.32), der Rest laeuft normal weiter.
        return 0.15f;
    default:
        // Timer-Phrasen: das globale 0.05 ist fuer sie schon knapp.
        return 0.0f;
    }
}

// Mehrere Phrasen pro Command-ID sind erlaubt (phrase_id unterscheidet
// sie). phonemes == NULL -> On-Device-G2P aus dem Text.
static const struct {
    command_id_t id;
    const char *phrase;
    const char *phonemes;
} COMMANDS[] = {
    { CMD_TIMER_1MIN,   "set timer one minute",      NULL },
    { CMD_TIMER_1MIN,   "one minute timer",          "WcN MgNcT TiMk" },
    { CMD_TIMER_5MIN,   "set timer five minutes",    "SfT TiMk FiV MgNcT" },
    { CMD_TIMER_5MIN,   "five minute timer",         "FiV MgNcT TiMk" },
    { CMD_TIMER_10MIN,  "set timer ten minutes",     "SfT TiMk TfN MgNcT" },
    { CMD_TIMER_10MIN,  "ten minute timer",          "TfN MgNcT TiMk" },
    { CMD_TIMER_15MIN,  "set timer fifteen minutes", "SfT TiMk FgFTmN MgNcT" },
    { CMD_TIMER_15MIN,  "fifteen minute timer",      "FgFTmN MgNcT TiMk" },
    { CMD_TIMER_30MIN,  "set timer thirty minutes",  "SfT TiMk vkDm MgNcT" },
    { CMD_TIMER_30MIN,  "thirty minute timer",       "vkDm MgNcT TiMk" },
    { CMD_TIMER_30SEC,  "set timer thirty seconds",  NULL },
    { CMD_TIMER_30SEC,  "thirty second timer",       NULL },
    // Sekunden-Timer (2026-07-02): explizite Phoneme, finales "Z" von
    // "seconds" weggelassen (analog MgNcTS->MgNcT bei "minutes"). Ohne
    // diese matcht "N seconds" den Minuten-Command gleichen Namens.
    { CMD_TIMER_5SEC,   "set timer five seconds",    "SfT TiMk FiV SfKcND" },
    { CMD_TIMER_5SEC,   "five second timer",         "FiV SfKcND TiMk" },
    { CMD_TIMER_10SEC,  "set timer ten seconds",     "SfT TiMk TfN SfKcND" },
    { CMD_TIMER_10SEC,  "ten second timer",          "TfN SfKcND TiMk" },
    { CMD_TIMER_15SEC,  "set timer fifteen seconds", "SfT TiMk FgFTmN SfKcND" },
    { CMD_TIMER_15SEC,  "fifteen second timer",      "FgFTmN SfKcND TiMk" },
    { CMD_TIMER_STOP,   "stop timer",                NULL },
    { CMD_TIMER_CANCEL, "cancel timer",              NULL },
    { CMD_TIME_LEFT,    "how much time left",        NULL },
    { CMD_STOP,         "stop",                      NULL },
    { CMD_VOLUME_UP,    "volume up",                 NULL },
    { CMD_VOLUME_DOWN,  "volume down",               NULL },
    { CMD_MUTE,         "mute",                      NULL },
    { CMD_UNMUTE,       "unmute",                    NULL },
    // Phase 8b: direkte Volume-Stufen (20-%-Schritte von volume.c).
    // Explizite Phoneme vom Host-G2P (multinet_g2p.py, wie bei den
    // Timer-Phrasen): mit On-Device-G2P matchte "volume sixty percent"
    // nie — gleiche Fehlerklasse wie "minute"/"MgNoT" (Session 6).
    { CMD_VOLUME_20,    "volume twenty percent",  "VnLYoM TWfNTm PkSfNT" },
    { CMD_VOLUME_40,    "volume forty percent",   "VnLYoM FeRTm PkSfNT" },
    // "sixty" als ein Token (SgKSTm) matcht nie — vermutlich im FST
    // defekt wie MgNcTS; gesplittet ("six tea" = SgKS Tm) geht es.
    // Dazu Aussprache-Aliasse (User-Test 2026-06-12: echte Stimme
    // matchte nie, say-Stimmen schon): stimmhaftes S wie im Deutschen
    // ("zixty"), offenes/langes i ("sexty"/"seexty").
    { CMD_VOLUME_60,    "volume sixty percent",   "VnLYoM SgKS Tm PkSfNT" },
    { CMD_VOLUME_60,    "volume zixty percent",   "VnLYoM ZgKS Tm PkSfNT" },
    { CMD_VOLUME_60,    "volume sexty percent",   "VnLYoM SfKS Tm PkSfNT" },
    { CMD_VOLUME_60,    "volume seexty percent",  "VnLYoM SmKS Tm PkSfNT" },
    { CMD_VOLUME_80,    "volume eighty percent",  "VnLYoM dTm PkSfNT" },
    { CMD_VOLUME_100,   "volume one hundred percent",
                                                  "VnLYoM WcN hcNDRcD PkSfNT" },
    // Temperatur: Phoneme per multinet_g2p.py (2026-07-01); On-Device-
    // G2P misshandelt "temperature" aehnlich wie "minute" (MgNoT).
    { CMD_TEMPERATURE,  "temperature",                      "TfMPRcpk" },
    { CMD_TEMPERATURE,  "what is the temperature",          "WcT gZ jc TfMPRcpk" },
    { CMD_TEMPERATURE,  "current temperature",              "KkcNT TfMPRcpk" },
    { CMD_TEMPERATURE,  "whats the temperature",            "WcTS jc TfMPRcpk" },
    { CMD_TEMPERATURE,  "whats the current temperature",    "WcTS jc KkcNT TfMPRcpk" },
    { CMD_TEMPERATURE,  "how warm is it",                   "ht WeRM gZ gT" },
    // Charakter-Wechsel (Phase B2): Phoneme per multinet_g2p.py. Der
    // Standalone-Name diskriminiert am besten (kein geteilter Prefix wie
    // bei "switch to X", wo MultiNet den Namen kaum trennt). Self-Trigger
    // durch das eigene Wake Word ist unkritisch: das aktive Wakeword ==
    // aktiver Charakter -> apply_character macht daraus einen No-Op.
    // "alexa" mit zwei Aussprachen (Schwa-Anfang vs. klares 'a').
    { CMD_CHARACTER_MYCROFT, "mycroft",           "MiKReFT" },
    { CMD_CHARACTER_MYCROFT, "switch to mycroft", "SWgp To MiKReFT" },
    // Alexa DIAGNOSE (2026-07-01): mehrere Aussprache-Spellings gleichzeitig,
    // min_prob(ALEXA)=0, um im Log zu sehen, welche Variante bei gesprochenem
    // "Alexa" (deutsch: volle Vokale statt US-Schwa) mit welcher Prob matcht.
    // Schwache danach entfernen, min_prob wieder anheben.
    { CMD_CHARACTER_ALEXA,   "alexa",             "cLfKSc" },   // US G2P (Schwa)
    { CMD_CHARACTER_ALEXA,   "ahlexah",           "nLfKSn" },   // volle Vokale AA
    { CMD_CHARACTER_ALEXA,   "alexah",            "cLfKSn" },   // Schwa+volles a
    { CMD_CHARACTER_ALEXA,   "ah lexa",           "n LfKSn" },  // gesplittet
    { CMD_CHARACTER_ALEXA,   "alexuh",            "aLcKso" },
    { CMD_CHARACTER_ALEXA,   "switch to alexa",   "SWgp To cLfKSc" },
    { CMD_CHARACTER_ALEXA,   "switch to ahlexah", "SWgp To nLfKSn" },
    { CMD_CHARACTER_JARVIS,  "jarvis",            "qnRVcS" },
    { CMD_CHARACTER_JARVIS,  "switch to jarvis",  "SWgp To qnRVcS" },
    // Zuverlaessiger namensunabhaengiger Cycle (Teil A): geht immer, auch
    // wenn ein Eigenname nicht getroffen wird.
    { CMD_CHARACTER_NEXT,    "next character",    "NfKST KfRgKTk" },
    { CMD_CHARACTER_NEXT,    "change character",  "pdNq KfRgKTk" },
    { CMD_CHARACTER_NEXT,    "next assistant",    "NfKST cSgSTcNT" },
    { CMD_CHARACTER_NEXT,    "change assistant",  "pdNq cSgSTcNT" },
};

static const esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_mn_data;
static commands_match_cb_t s_match_cb;
static commands_timeout_cb_t s_timeout_cb;

static int16_t *s_chunk;        // Sammel-Buffer: 1 MultiNet-Chunk
static size_t s_chunk_samples;  // dessen Groesse
static size_t s_chunk_fill;
static bool s_active;

esp_err_t commands_init(srmodel_list_t *models, commands_match_cb_t cb,
                        commands_timeout_cb_t timeout_cb)
{
    char *mn_name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX,
                                                ESP_MN_ENGLISH)
                           : NULL;
    if (!mn_name) {
        ESP_LOGE(TAG, "no English MultiNet in model partition");
        return ESP_FAIL;
    }

    s_multinet = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_multinet->create(mn_name, MN_TIMEOUT_MS);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed (%s)", mn_name);
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mn_commands_alloc((esp_mn_iface_t *)s_multinet,
                                          s_mn_data));
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (COMMANDS[i].phonemes) {
            ESP_ERROR_CHECK(esp_mn_commands_phoneme_add(
                COMMANDS[i].id, COMMANDS[i].phrase, COMMANDS[i].phonemes));
        } else {
            ESP_ERROR_CHECK(esp_mn_commands_add(COMMANDS[i].id,
                                                COMMANDS[i].phrase));
        }
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num > 0) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGE(TAG, "rejected phrase: \"%s\"",
                     err->phrases[i]->string);
        }
        return ESP_FAIL;
    }

    s_multinet->set_det_threshold(s_mn_data, MN_DET_THRESHOLD);

    s_chunk_samples = (size_t)s_multinet->get_samp_chunksize(s_mn_data);
    s_chunk = heap_caps_calloc(s_chunk_samples, sizeof(int16_t),
                               MALLOC_CAP_INTERNAL);
    if (!s_chunk) {
        return ESP_ERR_NO_MEM;
    }

    // Warmup: ~0,5 s Stille durch detect() schieben, damit der erste
    // echte Command nicht auf kalten Caches laeuft.
    for (int i = 0; i < 16; i++) {
        s_multinet->detect(s_mn_data, s_chunk);
    }
    s_multinet->clean(s_mn_data);

    s_match_cb = cb;
    s_timeout_cb = timeout_cb;
    ESP_LOGI(TAG, "%s ready: %d phrases, chunk %u samples, timeout %d ms",
             mn_name, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
             (unsigned)s_chunk_samples, MN_TIMEOUT_MS);
    return ESP_OK;
}

void commands_session_start(void)
{
    s_multinet->clean(s_mn_data);
    s_chunk_fill = 0;
    s_active = true;
}

void commands_session_stop(void)
{
    s_active = false;
}

bool commands_session_active(void)
{
    return s_active;
}

static void detect_chunk(int16_t *chunk)
{
    esp_mn_state_t state = s_multinet->detect(s_mn_data, chunk);
    if (state == ESP_MN_STATE_DETECTING) {
        return;
    }

    if (state == ESP_MN_STATE_TIMEOUT) {
        s_active = false;
        ESP_LOGI(TAG, "no local command (timeout) -> cloud query");
        if (s_timeout_cb) {
            s_timeout_cb();
        }
        return;
    }

    esp_mn_results_t *res = s_multinet->get_results(s_mn_data);
    if (res->num <= 0) {
        s_active = false;
        return;
    }

    command_id_t id = (command_id_t)res->command_id[0];
    float prob = res->prob[0];
    if (prob < min_prob(id)) {
        // Mutmasslicher False-Positive (z.B. deutscher Satz, der einen
        // Kurz-Command anklingt): Match verwerfen und WEITERLAUSCHEN —
        // der Rest des Satzes laeuft in den naechsten Detect-Zyklus,
        // der Cloud-Pfad bleibt erhalten. clean() startet auch das
        // 6-s-Timeout neu (verschiebt den Cloud-Upload entsprechend).
        ESP_LOGW(TAG, "rejecting \"%s\" (prob %.2f < %.2f), listening on",
                 esp_mn_commands_get_string(id), prob, min_prob(id));
        s_multinet->clean(s_mn_data);
        return;
    }

    s_active = false;
    ESP_LOGI(TAG, "command %d \"%s\" (prob %.2f)", id,
             esp_mn_commands_get_string(id), prob);
    if (s_match_cb) {
        s_match_cb(id, res->string, prob);
    }
}

void commands_feed(const int16_t *data, size_t samples)
{
    while (s_active && samples > 0) {
        size_t n = s_chunk_samples - s_chunk_fill;
        if (n > samples) {
            n = samples;
        }
        memcpy(s_chunk + s_chunk_fill, data, n * sizeof(int16_t));
        s_chunk_fill += n;
        data += n;
        samples -= n;

        if (s_chunk_fill == s_chunk_samples) {
            s_chunk_fill = 0;
            detect_chunk(s_chunk);
        }
    }
}
