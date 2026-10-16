#include "i18n.h"

#include <stdio.h>
#include <string.h>

#include "weather.h"

static const char *const CODES[LANG_N]   = { "en", "es", "fr", "pt", "it", "de", "tl" };
static const char *const NAMES[LANG_N]   = { "English", "Español", "Français", "Português", "Italiano", "Deutsch", "Tagalog" };
static const char *const ENGLISH[LANG_N] = { "English", "Spanish", "French", "Portuguese", "Italian", "German", "Tagalog" };

int lang_find(const char *code)
{
    if (!code || !code[0]) return LANG_EN;
    for (int i = 0; i < LANG_N; i++)
        if (strcmp(code, CODES[i]) == 0) return i;
    return -1;
}

int lang_from_code(const char *code)
{
    int l = lang_find(code);
    return l < 0 ? LANG_EN : l;
}

static int ok(int lang) { return lang >= 0 && lang < LANG_N ? lang : LANG_EN; }

const char *lang_code(int lang) { return CODES[ok(lang)]; }
const char *lang_name(int lang) { return NAMES[ok(lang)]; }
const char *lang_english(int lang) { return ENGLISH[ok(lang)]; }

/* Each row: English, Spanish, French, Portuguese, Italian, German, Tagalog.
   Spoken to older adults, so the polite forms: usted, vous, Lei, Sie, po. */
static const char *const T[TR_N][LANG_N] = {
    [TR_TALK]        = { "Talk", "Hablar", "Parler", "Falar", "Parli", "Sprechen", "Magsalita" },
    [TR_TALK_HINT]   = { "tap, or press BOOT", "toque, o pulse BOOT", "touchez, ou appuyez sur BOOT", "toque, ou aperte BOOT",
                         "tocchi, o prema BOOT", "tippen oder BOOT drücken", "i-tap, o pindutin ang BOOT" },
    [TR_NO_WIFI]     = { "no WiFi", "sin WiFi", "pas de WiFi", "sem WiFi", "niente WiFi", "kein WLAN", "walang WiFi" },

    [TR_BANNER]      = { "LISTENING", "ESCUCHANDO", "J'ÉCOUTE", "OUVINDO", "IN ASCOLTO", "ICH HÖRE ZU", "NAKIKINIG" },
    [TR_LISTENING]   = { "Go ahead, I'm listening", "Adelante, le escucho", "Allez-y, je vous écoute", "Pode falar, estou ouvindo",
                         "Prego, La ascolto", "Bitte, ich höre zu", "Sige po, nakikinig ako" },
    [TR_YOUR_TURN]   = { "Your turn", "Su turno", "À vous", "Sua vez", "Tocca a Lei", "Sie sind dran", "Kayo naman po" },
    [TR_HOW_ARE_YOU] = { "How are you today?", "¿Cómo está hoy?", "Comment allez-vous ?", "Como vai hoje?", "Come sta oggi?",
                         "Wie geht es Ihnen heute?", "Kumusta po kayo ngayon?" },
    [TR_PAUSE_HINT]  = { "pause when you're done  -  tap to end", "haga una pausa al terminar  -  toque para acabar",
                         "faites une pause à la fin  -  touchez pour finir", "faça uma pausa ao terminar  -  toque para encerrar",
                         "faccia una pausa alla fine  -  tocchi per finire", "Pause, wenn Sie fertig sind  -  tippen zum Beenden",
                         "huminto kapag tapos na  -  i-tap para tapusin" },
    [TR_KEEP_TALKING] = { "or just keep talking", "o siga hablando", "ou continuez à parler", "ou continue falando",
                          "o continui pure a parlare", "oder einfach weitersprechen", "o magpatuloy lang po" },
    [TR_AGAIN]       = { "Again", "Otra vez", "Encore", "De novo", "Ancora", "Nochmal", "Ulitin" },
    [TR_SLOWER]      = { "Slower", "Despacio", "Plus lent", "Devagar", "Piano", "Langsam", "Mabagal" },
    [TR_DONE]        = { "Done", "Listo", "Fini", "Pronto", "Finito", "Fertig", "Tapos" },
    [TR_AGAIN_SUB]   = { "say it again", "repetir", "répéter", "repetir", "ripetere", "wiederholen", "ulitin muli" },
    [TR_SLOWER_SUB]  = { "repeat slowly", "más despacio", "plus lentement", "mais devagar", "più lentamente", "langsamer",
                         "dahan-dahan" },
    [TR_DONE_SUB]    = { "I'm finished", "he terminado", "j'ai fini", "terminei", "ho finito", "ich bin fertig", "tapos na ako" },
    [TR_STOP]        = { "Stop", "Parar", "Arrêter", "Parar", "Ferma", "Stopp", "Itigil" },
    [TR_SHALL_I]     = { "Shall I go ahead?", "¿Sigo adelante?", "Je continue ?", "Posso continuar?", "Procedo?",
                         "Soll ich das tun?", "Itutuloy ko na po ba?" },
    [TR_YES]         = { "Yes", "Sí", "Oui", "Sim", "Sì", "Ja", "Oo" },
    [TR_NO]          = { "No", "No", "Non", "Não", "No", "Nein", "Hindi" },
    [TR_SAY_YES_NO]  = { "or just say yes or no", "o diga sí o no", "ou dites oui ou non", "ou diga sim ou não",
                         "o dica sì o no", "oder sagen Sie Ja oder Nein", "o sabihin lang ang oo o hindi" },
    [TR_THINKING]    = { "Thinking...", "Pensando...", "Je réfléchis...", "Pensando...", "Ci penso...", "Ich überlege...",
                         "Nag-iisip..." },

    [TR_OK]          = { "OK", "OK", "OK", "OK", "OK", "OK", "Sige" },
    [TR_REMINDER]    = { "Reminder", "Recordatorio", "Rappel", "Lembrete", "Promemoria", "Erinnerung", "Paalala" },
    [TR_MESSAGE]     = { "Message", "Mensaje", "Message", "Mensagem", "Messaggio", "Nachricht", "Mensahe" },
    [TR_A_PHOTO]     = { "A photo", "Una foto", "Une photo", "Uma foto", "Una foto", "Ein Foto", "Isang larawan" },
    [TR_MSG_FROM]    = { "Message from %s", "Mensaje de %s", "Message de %s", "Mensagem de %s", "Messaggio da %s",
                         "Nachricht von %s", "Mensahe mula kay %s" },
    [TR_MSG_FAMILY]  = { "Message from family", "Mensaje de la familia", "Message de la famille", "Mensagem da família",
                         "Messaggio dalla famiglia", "Nachricht von der Familie", "Mensahe mula sa pamilya" },
    [TR_PHOTO_FROM]  = { "A photo from %s", "Una foto de %s", "Une photo de %s", "Uma foto de %s", "Una foto da %s",
                         "Ein Foto von %s", "Larawan mula kay %s" },
    [TR_PHOTO_FAMILY] = { "A photo from family", "Una foto de la familia", "Une photo de la famille", "Uma foto da família",
                          "Una foto dalla famiglia", "Ein Foto von der Familie", "Larawan mula sa pamilya" },
    [TR_SENT_PHOTO]  = { "%s sent you a photo.", "%s le envió una foto.", "%s vous a envoyé une photo.",
                         "%s enviou uma foto para você.", "%s Le ha mandato una foto.", "%s hat Ihnen ein Foto geschickt.",
                         "Nagpadala si %s ng larawan." },
    [TR_FAMILY_SENT_PHOTO] = { "Your family sent you a photo.", "Su familia le envió una foto.",
                               "Votre famille vous a envoyé une photo.", "Sua família enviou uma foto para você.",
                               "La Sua famiglia Le ha mandato una foto.", "Ihre Familie hat Ihnen ein Foto geschickt.",
                               "Nagpadala ang pamilya ninyo ng larawan." },
    [TR_FROM]        = { "from %s", "de %s", "de %s", "de %s", "da %s", "von %s", "mula kay %s" },

    [TR_TODAY]       = { "TODAY", "HOY", "AUJOURD'HUI", "HOJE", "OGGI", "HEUTE", "NGAYON" },
    [TR_TOMORROW]    = { "TOMORROW", "MAÑANA", "DEMAIN", "AMANHÃ", "DOMANI", "MORGEN", "BUKAS" },
    [TR_FEELS_LIKE]  = { "Feels like", "Sensación", "Ressenti", "Sensação", "Percepita", "Gefühlt", "Pakiramdam" },

    [TR_ANSWER_PREVIEW] = { "This is how answers look.", "Así se ven las respuestas.", "Voici à quoi ressemblent les réponses.",
                            "Assim ficam as respostas.", "Ecco come appaiono le risposte.", "So sehen Antworten aus.",
                            "Ganito ang hitsura ng sagot." },

    [TR_SETUP_TITLE] = { "Set up WiFi", "Configurar WiFi", "Configurer le WiFi", "Configurar WiFi", "Configura il WiFi",
                         "WLAN einrichten", "I-set up ang WiFi" },
    [TR_YOUR_WIFI]   = { "your WiFi", "su WiFi", "votre WiFi", "seu WiFi", "il Suo WiFi", "Ihr WLAN", "ang WiFi ninyo" },
    [TR_CONNECTED]   = { "Connected to %s", "Conectado a %s", "Connecté à %s", "Conectado a %s", "Connesso a %s",
                         "Verbunden mit %s", "Nakakonekta sa %s" },
    [TR_CONNECTING]  = { "Connecting to %s...", "Conectando a %s...", "Connexion à %s...", "Conectando a %s...",
                         "Connessione a %s...", "Verbinde mit %s...", "Kumokonekta sa %s..." },
    [TR_PHONE_AWAY]  = { "You can put the phone away.", "Ya puede guardar el teléfono.", "Vous pouvez ranger le téléphone.",
                         "Já pode guardar o telefone.", "Può mettere via il telefono.", "Sie können das Handy weglegen.",
                         "Puwede nang itabi ang telepono." },
    [TR_HALF_MINUTE] = { "This takes about half a minute.", "Tarda como medio minuto.", "Cela prend une demi-minute.",
                         "Leva cerca de meio minuto.", "Ci vuole circa mezzo minuto.", "Das dauert etwa eine halbe Minute.",
                         "Mga kalahating minuto ito." },
    [TR_SETUP_DONE]  = { "Done", "Listo", "Terminé", "Pronto", "Fatto", "Fertig", "Tapos" },
    [TR_STEP1]       = { "Point your phone's camera at the code", "Apunte la cámara al código",
                         "Visez le code avec le téléphone", "Aponte a câmera do telefone para o código",
                         "Inquadri il codice col telefono", "Die Handykamera auf den Code richten",
                         "Itutok ang camera ng telepono sa code" },
    [TR_STEP2]       = { "Tap to join %s", "Toque para unirse a %s", "Rejoignez %s", "Toque para entrar em %s",
                         "Si colleghi a %s", "Mit %s verbinden", "I-tap para sumali sa %s" },
    [TR_SETUP_NET]   = { "the setup network", "la red de configuración", "le réseau de configuration", "a rede de configuração",
                         "la rete di configurazione", "das Einrichtungsnetz", "ang setup network" },
    [TR_STEP3]       = { "On the page that opens, pick your WiFi and type its password",
                         "En la página que se abre, elija su WiFi y escriba la contraseña",
                         "Sur la page qui s'ouvre, choisissez votre WiFi et tapez son mot de passe",
                         "Na página que abrir, escolha seu WiFi e digite a senha",
                         "Nella pagina che si apre, scelga il WiFi e scriva la password",
                         "Auf der neuen Seite Ihr WLAN wählen und das Passwort eingeben",
                         "Sa page, piliin ang WiFi ninyo at i-type ang password" },
    [TR_COULDNT_JOIN] = { "Couldn't join %s.", "No se pudo unir a %s.", "Impossible de rejoindre %s.", "Não deu para entrar em %s.",
                          "Non riesco a collegarmi a %s.", "%s nicht erreicht.", "Hindi makasali sa %s." },
    [TR_CHECK_PASS1] = { "Check the password, and send it again", "Revise la contraseña y envíela otra vez",
                         "Vérifiez le mot de passe et renvoyez-le", "Confira a senha e envie de novo",
                         "Controlli la password e la invii di nuovo", "Passwort prüfen, dann erneut senden",
                         "Tingnan ang password at ipadala ulit" },
    [TR_CHECK_PASS2] = { "from the page on the phone.", "desde la página del teléfono.", "depuis la page sur le téléphone.",
                         "pela página no telefone.", "dalla pagina sul telefono.", "von der Seite auf dem Handy.",
                         "mula sa page sa telepono." },
    [TR_NO_CAMERA]   = { "No camera? In the phone's WiFi settings join", "¿Sin cámara? En el WiFi del teléfono, únase a",
                         "Pas d'appareil photo ? Dans le WiFi, rejoignez", "Sem câmera? No WiFi do telefone, entre em",
                         "Niente fotocamera? Nel WiFi del telefono scelga", "Keine Kamera? Im WLAN des Handys wählen Sie",
                         "Walang camera? Sa WiFi ng telepono, sumali sa" },
    [TR_PASSWORD]    = { "password %s", "contraseña %s", "mot de passe %s", "senha %s", "password %s", "Passwort %s",
                         "password %s" },
    [TR_THEN_OPEN]   = { "then open 192.168.4.1", "luego abra 192.168.4.1", "puis ouvrez 192.168.4.1", "depois abra 192.168.4.1",
                         "poi apra 192.168.4.1", "dann 192.168.4.1 öffnen", "tapos buksan ang 192.168.4.1" },

    [TR_ANYTHING_ELSE] = { "Is there anything else I can help you with?", "¿Hay algo más en lo que pueda ayudarle?",
                           "Puis-je vous aider pour autre chose ?", "Posso ajudar em mais alguma coisa?",
                           "Posso aiutarLa in qualcos'altro?", "Kann ich Ihnen sonst noch helfen?",
                           "May iba pa po ba akong maitutulong?" },
    [TR_GOODBYE]     = { "All right. Just tap the screen whenever you need me. Goodbye.",
                         "Muy bien. Toque la pantalla cuando me necesite. Adiós.",
                         "Très bien. Touchez l'écran quand vous aurez besoin de moi. Au revoir.",
                         "Tudo bem. É só tocar na tela quando precisar de mim. Tchau.",
                         "Va bene. Tocchi lo schermo quando ha bisogno di me. Arrivederci.",
                         "In Ordnung. Tippen Sie einfach auf den Bildschirm, wenn Sie mich brauchen. Auf Wiedersehen.",
                         "Sige po. I-tap lang ang screen kapag kailangan ninyo ako. Paalam." },
    [TR_CHECKIN_MORNING] = { "Good morning! I'm just checking in. How are you today?",
                             "¡Buenos días! Solo quería saludarle. ¿Cómo está hoy?",
                             "Bonjour ! Je viens prendre de vos nouvelles. Comment allez-vous aujourd'hui ?",
                             "Bom dia! Só queria saber de você. Como vai hoje?",
                             "Buongiorno! Passo solo a salutarLa. Come sta oggi?",
                             "Guten Morgen! Ich wollte nur kurz nachfragen. Wie geht es Ihnen heute?",
                             "Magandang umaga po! Kinukumusta ko lang kayo. Kumusta po kayo ngayon?" },
    [TR_CHECKIN_DAY] = { "Hello! I'm just checking in. How are you today?",
                         "¡Hola! Solo quería saludarle. ¿Cómo está hoy?",
                         "Bonjour ! Je viens prendre de vos nouvelles. Comment allez-vous ?",
                         "Olá! Só queria saber de você. Como vai hoje?",
                         "Salve! Passo solo a salutarLa. Come sta oggi?",
                         "Hallo! Ich wollte nur kurz nachfragen. Wie geht es Ihnen heute?",
                         "Magandang araw po! Kinukumusta ko lang kayo. Kumusta po kayo ngayon?" },
    [TR_ENDED]       = { "conversation ended", "conversación terminada", "conversation terminée", "conversa encerrada",
                         "conversazione finita", "Gespräch beendet", "tapos na ang usapan" },
    [TR_MISSED]      = { "didn't catch that", "no le entendí", "je n'ai pas compris", "não entendi", "non ho capito",
                         "nicht verstanden", "hindi ko narinig" },
    [TR_HOA_CHECKED] = { "I checked your HOA records", "Revisé sus registros de la HOA", "J'ai consulté votre dossier HOA",
                         "Consultei seus registros da HOA", "Ho controllato i Suoi dati HOA", "Ich habe Ihre HOA-Daten geprüft",
                         "Tiningnan ko ang HOA records ninyo" },
    [TR_HOA_FAILED]  = { "I couldn't reach the HOA service just now", "No pude contactar el servicio de la HOA",
                         "Je n'ai pas pu joindre le service HOA", "Não consegui acessar o serviço da HOA",
                         "Non ho raggiunto il servizio HOA", "Der HOA-Dienst war nicht erreichbar",
                         "Hindi ko maabot ang HOA service ngayon" },

    [TR_PH_MOMENT1]  = { "One moment, let me check that for you.", "Un momento, lo reviso enseguida.",
                         "Un instant, je vérifie pour vous.", "Um momento, vou verificar para você.",
                         "Un momento, controllo subito.", "Einen Moment, ich sehe kurz nach.", "Sandali lang po, titingnan ko." },
    [TR_PH_MOMENT2]  = { "Just a moment, I'm looking that up.", "Un momentito, lo estoy buscando.", "Juste un instant, je cherche.",
                         "Só um momento, estou procurando.", "Solo un attimo, sto cercando.",
                         "Nur einen Augenblick, ich schaue nach.", "Saglit lang po, hinahanap ko." },
    [TR_PH_MOMENT3]  = { "Bear with me a second, I'm checking.", "Deme un segundo, estoy revisando.", "Une seconde, je regarde.",
                         "Só um segundo, estou conferindo.", "Mi dia un secondo, sto verificando.",
                         "Eine Sekunde bitte, ich prüfe das.", "Isang segundo lang po, sinusuri ko." },
    [TR_PH_OFFLINE]  = { "I can't reach the internet right now. Your WiFi may be down. I'll keep trying, so please try "
                         "again in a few minutes.",
                         "Ahora no puedo conectarme a internet. Quizá su WiFi no funciona. Seguiré intentándolo: pruebe otra "
                         "vez en unos minutos.",
                         "Je n'arrive pas à joindre internet. Votre WiFi est peut-être coupé. Je continue d'essayer : "
                         "réessayez dans quelques minutes.",
                         "Não consigo acessar a internet agora. Seu WiFi pode estar fora do ar. Vou continuar tentando: tente "
                         "de novo em alguns minutos.",
                         "Ora non riesco a collegarmi a internet. Forse il WiFi non funziona. Continuo a provare: riprovi tra "
                         "qualche minuto.",
                         "Ich erreiche das Internet gerade nicht. Vielleicht ist Ihr WLAN aus. Ich versuche es weiter: bitte "
                         "in ein paar Minuten noch einmal.",
                         "Hindi ko maabot ang internet ngayon. Baka sira ang WiFi ninyo. Susubukan ko pa rin, kaya pakiulit "
                         "pagkalipas ng ilang minuto." },
    [TR_PH_TROUBLE]  = { "I'm having trouble reaching my helpers right now. Please try again in a minute.",
                         "Tengo problemas para contactar a mis ayudantes. Por favor, pruebe otra vez en un minuto.",
                         "J'ai du mal à joindre mes services en ce moment. Réessayez dans une minute, s'il vous plaît.",
                         "Estou com dificuldade para falar com meus serviços. Por favor, tente de novo em um minuto.",
                         "Ho difficoltà a raggiungere i miei servizi. Riprovi tra un minuto, per favore.",
                         "Ich erreiche meine Dienste gerade nicht. Bitte versuchen Sie es in einer Minute noch einmal.",
                         "Nahihirapan akong maabot ang mga katulong ko ngayon. Pakisubukan ulit pagkalipas ng isang minuto." },
    [TR_PH_MISSED]   = { "Sorry, I didn't hear anything. Just tap the screen when you're ready to talk.",
                         "Perdone, no oí nada. Toque la pantalla cuando quiera hablar.",
                         "Pardon, je n'ai rien entendu. Touchez l'écran quand vous voulez parler.",
                         "Desculpe, não ouvi nada. Toque na tela quando quiser falar.",
                         "Mi scusi, non ho sentito niente. Tocchi lo schermo quando vuole parlare.",
                         "Entschuldigung, ich habe nichts gehört. Tippen Sie auf den Bildschirm, wenn Sie sprechen möchten.",
                         "Paumanhin po, wala akong narinig. I-tap lang ang screen kapag handa na kayong magsalita." },
};

const char *tr(int lang, int key)
{
    if (key < 0 || key >= TR_N) return "";
    const char *s = T[key][ok(lang)];
    return s ? s : T[key][LANG_EN] ? T[key][LANG_EN] : "";
}

/* weather_words' phrases, each row in the seven languages, English first. */
static const char *const WX[][LANG_N] = {
    { "Clear", "Despejado", "Dégagé", "Céu limpo", "Sereno", "Klar", "Maaliwalas" },
    { "Mainly clear", "Casi despejado", "Plutôt dégagé", "Quase limpo", "Poco nuvoloso", "Überwiegend klar", "Halos maaliwalas" },
    { "Partly cloudy", "Algo nublado", "Éclaircies", "Parcialmente nublado", "Parz. nuvoloso", "Teils bewölkt", "Bahagyang maulap" },
    { "Cloudy", "Nublado", "Nuageux", "Nublado", "Nuvoloso", "Bewölkt", "Maulap" },
    { "Fog", "Niebla", "Brouillard", "Nevoeiro", "Nebbia", "Nebel", "Hamog" },
    { "Drizzle", "Llovizna", "Bruine", "Garoa", "Pioggerella", "Nieselregen", "Ambon" },
    { "Light rain", "Lluvia ligera", "Pluie légère", "Chuva fraca", "Pioggia leggera", "Leichter Regen", "Mahinang ulan" },
    { "Rain", "Lluvia", "Pluie", "Chuva", "Pioggia", "Regen", "Ulan" },
    { "Heavy rain", "Lluvia fuerte", "Forte pluie", "Chuva forte", "Pioggia forte", "Starker Regen", "Malakas na ulan" },
    { "Freezing rain", "Lluvia helada", "Pluie verglaçante", "Chuva gelada", "Pioggia gelata", "Eisregen", "Nagyeyelong ulan" },
    { "Light snow", "Nieve ligera", "Neige légère", "Neve fraca", "Neve leggera", "Leichter Schnee", "Mahinang niyebe" },
    { "Snow", "Nieve", "Neige", "Neve", "Neve", "Schnee", "Niyebe" },
    { "Thunderstorms", "Tormentas", "Orages", "Trovoadas", "Temporali", "Gewitter", "Kulog at kidlat" },
    { "Mixed", "Variable", "Variable", "Variável", "Variabile", "Wechselhaft", "Pabagu-bago" },
};

const char *tr_weather(int lang, int code)
{
    const char *en = weather_words(code);
    for (size_t i = 0; i < sizeof WX / sizeof WX[0]; i++)
        if (strcmp(en, WX[i][0]) == 0) return WX[i][ok(lang)];
    return en;
}

/* The days, the parts of the day (morning, afternoon, evening, night) and
   how they join, the months, and which comes first in a date. Months are
   lower case where the language writes them so mid-sentence. */
static const struct {
    const char *day[7];
    const char *when[4];
    const char *join;          /* day, then the part: "%s %s", "%s%s" (German makes one word), "%s ng %s" */
    const char *month[12];
    const char *between;       /* between the day of the month and the month, day first; NULL: month first, "October 10" */
} DAYS[LANG_N] = {
    [LANG_EN] = { { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
                  { "morning", "afternoon", "evening", "night" }, "%s %s",
                  { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
                    "November", "December" }, NULL },
    [LANG_ES] = { { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" },
                  { "por la mañana", "por la tarde", "por la noche", "por la noche" }, "%s %s",
                  { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre",
                    "noviembre", "diciembre" }, " de " },
    [LANG_FR] = { { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" },
                  { "matin", "après-midi", "soir", "nuit" }, "%s %s",
                  { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre",
                    "novembre", "décembre" }, " " },
    [LANG_PT] = { { "Domingo", "Segunda-feira", "Terça-feira", "Quarta-feira", "Quinta-feira", "Sexta-feira", "Sábado" },
                  { "de manhã", "à tarde", "à noite", "à noite" }, "%s %s",
                  { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro",
                    "novembro", "dezembro" }, " de " },
    [LANG_IT] = { { "Domenica", "Lunedì", "Martedì", "Mercoledì", "Giovedì", "Venerdì", "Sabato" },
                  { "mattina", "pomeriggio", "sera", "notte" }, "%s %s",
                  { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre",
                    "novembre", "dicembre" }, " " },
    [LANG_DE] = { { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
                  { "morgen", "nachmittag", "abend", "nacht" }, "%s%s",
                  { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober",
                    "November", "Dezember" }, ". " },
    [LANG_TL] = { { "Linggo", "Lunes", "Martes", "Miyerkules", "Huwebes", "Biyernes", "Sabado" },
                  { "umaga", "hapon", "gabi", "gabi" }, "%s ng %s",
                  { "Enero", "Pebrero", "Marso", "Abril", "Mayo", "Hunyo", "Hulyo", "Agosto", "Setyembre", "Oktubre",
                    "Nobyembre", "Disyembre" }, NULL },
};

void tr_day_words(int lang, int wday, int month, int mday, int minute, char *part, size_t np, char *date, size_t nd)
{
    lang = ok(lang);
    if (np) part[0] = 0;
    if (nd) date[0] = 0;
    if (month < 1 || month > 12) return;
    int m = ((minute % 1440) + 1440) % 1440;
    int w = m >= 5 * 60 && m < 12 * 60 ? 0 : m >= 12 * 60 && m < 17 * 60 ? 1 : m >= 17 * 60 && m < 21 * 60 ? 2 : 3;
    const char *when = DAYS[lang].when[w], *mon = DAYS[lang].month[month - 1];
    if (np) {
        if (wday >= 0 && wday < 7) snprintf(part, np, DAYS[lang].join, DAYS[lang].day[wday], when);
        else snprintf(part, np, "%s", when);
    }
    if (nd) {
        if (mday <= 0) snprintf(date, nd, "%s", mon);
        else if (DAYS[lang].between) snprintf(date, nd, "%d%s%s", mday, DAYS[lang].between, mon);
        else snprintf(date, nd, "%s %d", mon, mday);
        if (date[0] >= 'a' && date[0] <= 'z') date[0] = (char)(date[0] - 'a' + 'A');   /* a month alone starts the line */
    }
}
