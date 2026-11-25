#ifndef AAP_I18N_H
#define AAP_I18N_H
/* UI localisation from the CIC's own language setting.
 *
 * Source: the HMI's CPxyManMachineInterface proxy (factory 0x08be9fb0, instance 1),
 * property CurrentLanguage at +0x750, read with the getter that
 * CIbaCommunication::onStatusCurrentLanguage uses (0x08bea500: returns 1 when the
 * property is valid and copies the ELanguage value). Addresses are for the E9x
 * build (HmiMain 9283417A) and go through iba_a(): on another build the language
 * is only read when its profile in iba_profiles.h translates them; otherwise the
 * texts stay in English.
 * ELanguage values, from the IBA language-code switch (0x082bc844): 1 de-de,
 * 2 en-gb, 3 en-us, 4 es, 5 it, 6 fr, 7/8 nl, 9 ar, 10 zh-tw, 11 zh-cn, 12 ko,
 * 13 ja, 14 ru, 15 fr(CA), 16 es(MX), 17 pt, 18 pl, 19 el, 20 tr, 21 hu, 22 ro, 23 sv.
 *
 * The English texts in the code are the keys, so internal str_eq() comparisons
 * keep working: only text sent to the HMI models is translated. Texts are ASCII
 * (no accents/umlauts): the CIC font path is not verified for UTF-8.
 * The fixed XML labels (buttons) cannot change at run time and stay in English.
 * Table generated from a row list (en, es, de, fr, it, nl, pt). */

enum AapLang { AAP_EN = 0, AAP_ES, AAP_DE, AAP_FR, AAP_IT, AAP_NL, AAP_PT, AAP_LANGS };

/* .bss (no static ctors on the CIC): 0 = English until read. */
static volatile int g_lang;        /* AapLang */
static int g_lang_raw;             /* last ELanguage logged (0 = none yet) */

static int aap_lang_from_cic(int e) {
    switch (e) {
    case 1: return AAP_DE;
    case 4: case 16: return AAP_ES;
    case 5: return AAP_IT;
    case 6: case 15: return AAP_FR;
    case 7: case 8: return AAP_NL;
    case 17: return AAP_PT;
    default: return AAP_EN;
    }
}

struct AapText { const char* t[AAP_LANGS]; };
static const AapText kAapTexts[] = {
 {{"Connect the phone via USB...", "Conecta el movil por USB...", "Telefon per USB verbinden...", "Branchez le telephone en USB...", "Collega il telefono via USB...", "Sluit de telefoon aan via USB...", "Ligue o telemovel por USB..."}},
 {{"Closing previous video, wait a few seconds", "Cerrando video anterior, espera unos segundos", "Vorheriges Video wird beendet, bitte warten", "Fermeture de la video precedente, patientez", "Chiusura video precedente, attendi qualche secondo", "Vorige video wordt gesloten, even geduld", "A fechar o video anterior, aguarde"}},
 {{"libcicaa.so missing (install)", "Falta libcicaa.so (instalar)", "libcicaa.so fehlt (installieren)", "libcicaa.so manquant (installer)", "Manca libcicaa.so (installare)", "libcicaa.so ontbreekt (installeren)", "Falta libcicaa.so (instalar)"}},
 {{"Could not load libcicaa.so", "No se pudo cargar libcicaa.so", "libcicaa.so konnte nicht geladen werden", "Impossible de charger libcicaa.so", "Impossibile caricare libcicaa.so", "Kan libcicaa.so niet laden", "Nao foi possivel carregar libcicaa.so"}},
 {{"libcicaa.so incompatible", "libcicaa.so incompatible", "libcicaa.so inkompatibel", "libcicaa.so incompatible", "libcicaa.so incompatibile", "libcicaa.so niet compatibel", "libcicaa.so incompativel"}},
 {{"Already running", "Ya en marcha", "Laeuft bereits", "Deja en cours", "Gia in esecuzione", "Draait al", "Ja em execucao"}},
 {{"Error starting AA", "Error arrancando AA", "Fehler beim Starten von AA", "Erreur au demarrage d'AA", "Errore avvio AA", "Fout bij starten AA", "Erro ao iniciar AA"}},
 {{"Error creating thread", "Error creando hilo", "Fehler beim Erstellen des Threads", "Erreur de creation du thread", "Errore creazione thread", "Fout bij maken thread", "Erro ao criar thread"}},
 {{"Could not load the patched DSP (reinstall)", "No se pudo cargar el DSP parcheado (reinstalar)", "Gepatchter DSP nicht ladbar (neu installieren)", "Impossible de charger le DSP modifie (reinstaller)", "Impossibile caricare il DSP modificato (reinstallare)", "Gepatchte DSP niet geladen (opnieuw installeren)", "Nao foi possivel carregar o DSP (reinstalar)"}},
 {{"Wrong HMI thread", "Hilo HMI distinto", "Falscher HMI-Thread", "Mauvais thread HMI", "Thread HMI errato", "Verkeerde HMI-thread", "Thread HMI errado"}},
 {{"Different HmiMain: not compatible", "HmiMain distinto: no compatible", "Anderes HmiMain: nicht kompatibel", "HmiMain different : incompatible", "HmiMain diverso: non compatibile", "Andere HmiMain: niet compatibel", "HmiMain diferente: incompativel"}},
 {{"CIC video services missing", "Faltan servicios de video del CIC", "CIC-Videodienste fehlen", "Services video du CIC absents", "Servizi video del CIC mancanti", "CIC-videodiensten ontbreken", "Faltam servicos de video do CIC"}},
 {{"Unexpected service layout", "Layout de servicios distinto", "Unerwartete Dienststruktur", "Structure des services inattendue", "Struttura servizi inattesa", "Onverwachte dienstindeling", "Estrutura de servicos inesperada"}},
 {{"Could not register listener", "No se pudo registrar listener", "Listener konnte nicht registriert werden", "Impossible d'enregistrer l'ecouteur", "Impossibile registrare il listener", "Kan listener niet registreren", "Nao foi possivel registar o listener"}},
 {{"Property subscription failed", "Suscripcion de propiedades fallo", "Eigenschaftsabo fehlgeschlagen", "Echec de l'abonnement aux proprietes", "Sottoscrizione proprieta fallita", "Abonnement op eigenschappen mislukt", "Subscricao de propriedades falhou"}},
 {{"Video screen busy with another source", "Pantalla de video ocupada por otra fuente", "Videobild von anderer Quelle belegt", "Ecran video occupe par une autre source", "Schermo video occupato da un'altra sorgente", "Videoscherm bezet door andere bron", "Ecra de video ocupado por outra fonte"}},
 {{"Unexpected video table", "Tabla de video distinta", "Unerwartete Videotabelle", "Table video inattendue", "Tabella video inattesa", "Onverwachte videotabel", "Tabela de video inesperada"}},
 {{"Waiting for video route", "Esperando ruta de video", "Warte auf Videopfad", "Attente du chemin video", "In attesa del percorso video", "Wachten op videoroute", "A aguardar rota de video"}},
 {{"Video route changed by another client", "Ruta de video cambiada por otro cliente", "Videopfad von anderem Client geaendert", "Chemin video modifie par un autre client", "Percorso video cambiato da un altro client", "Videoroute gewijzigd door andere client", "Rota de video alterada por outro cliente"}},
 {{"Video restore failed; restart the CIC", "Fallo restaurando video; reinicia el CIC", "Video-Wiederherstellung fehlgeschlagen; CIC neu starten", "Echec de restauration video ; redemarrez le CIC", "Ripristino video fallito; riavvia il CIC", "Video herstellen mislukt; herstart de CIC", "Falha ao restaurar video; reinicie o CIC"}},
 {{"Could not start cicaalive", "No se pudo arrancar cicaalive", "cicaalive konnte nicht gestartet werden", "Impossible de demarrer cicaalive", "Impossibile avviare cicaalive", "Kan cicaalive niet starten", "Nao foi possivel iniciar cicaalive"}},
 {{"cicaalive ended before showing video", "cicaalive termino antes de mostrar video", "cicaalive vor Videoanzeige beendet", "cicaalive s'est arrete avant la video", "cicaalive terminato prima del video", "cicaalive gestopt voor video zichtbaar was", "cicaalive terminou antes do video"}},
 {{"Action outside the HMI thread", "Accion fuera del hilo HMI", "Aktion ausserhalb des HMI-Threads", "Action hors du thread HMI", "Azione fuori dal thread HMI", "Actie buiten HMI-thread", "Acao fora do thread HMI"}},
 {{"Callback outside the HMI thread", "Callback fuera del hilo HMI", "Callback ausserhalb des HMI-Threads", "Callback hors du thread HMI", "Callback fuori dal thread HMI", "Callback buiten HMI-thread", "Callback fora do thread HMI"}},
 {{"cicaalive missing (reinstall)", "Falta cicaalive (reinstalar)", "cicaalive fehlt (neu installieren)", "cicaalive manquant (reinstaller)", "Manca cicaalive (reinstallare)", "cicaalive ontbreekt (opnieuw installeren)", "Falta cicaalive (reinstalar)"}},
 {{"Video layer busy with another client", "Capa de video ocupada por otro cliente", "Videoebene von anderem Client belegt", "Couche video occupee par un autre client", "Livello video occupato da un altro client", "Videolaag bezet door andere client", "Camada de video ocupada por outro cliente"}},
 {{"HMI thread not verified", "Hilo HMI no verificado", "HMI-Thread nicht verifiziert", "Thread HMI non verifie", "Thread HMI non verificato", "HMI-thread niet geverifieerd", "Thread HMI nao verificado"}},
 {{"CIC thread not verified.", "Hilo del CIC no verificado.", "CIC-Thread nicht verifiziert.", "Thread du CIC non verifie.", "Thread del CIC non verificato.", "CIC-thread niet geverifieerd.", "Thread do CIC nao verificado."}},
 {{"cicaalive not installed.", "Falta cicaalive instalado.", "cicaalive ist nicht installiert.", "cicaalive n'est pas installe.", "cicaalive non installato.", "cicaalive is niet geinstalleerd.", "cicaalive nao esta instalado."}},
 {{"CIC not responding; restart the CIC.", "El CIC no responde; reinicia el CIC.", "CIC reagiert nicht; CIC neu starten.", "Le CIC ne repond pas ; redemarrez-le.", "Il CIC non risponde; riavvia il CIC.", "De CIC reageert niet; herstart de CIC.", "O CIC nao responde; reinicie o CIC."}},
 {{"CIC not responding.", "El CIC no responde.", "CIC reagiert nicht.", "Le CIC ne repond pas.", "Il CIC non risponde.", "De CIC reageert niet.", "O CIC nao responde."}},
 {{"Cannot communicate with the CIC.", "No se puede comunicar con el CIC.", "Keine Kommunikation mit dem CIC.", "Communication impossible avec le CIC.", "Impossibile comunicare con il CIC.", "Kan niet communiceren met de CIC.", "Nao e possivel comunicar com o CIC."}},
 {{"Could not complete the operation.", "No se pudo completar la operacion.", "Vorgang konnte nicht abgeschlossen werden.", "L'operation n'a pas pu aboutir.", "Impossibile completare l'operazione.", "De bewerking kon niet worden voltooid.", "Nao foi possivel concluir a operacao."}},
 {{"Unknown player.", "Reproductor desconocido.", "Unbekannter Player.", "Lecteur inconnu.", "Lettore sconosciuto.", "Onbekende speler.", "Leitor desconhecido."}},
 {{"A session is active or a close is pending. Press Stop.", "Hay una sesion activa o un cierre pendiente. Pulsa Detener.", "Sitzung aktiv oder wird beendet. Stopp druecken.", "Session active ou fermeture en cours. Appuyez sur Arreter.", "Sessione attiva o in chiusura. Premi Stop.", "Sessie actief of wordt gesloten. Druk op Stop.", "Sessao ativa ou a fechar. Prima Parar."}},
 {{"Cannot prepare another session: close pending.", "No se puede preparar otra sesion: cierre pendiente.", "Keine neue Sitzung: Beenden ausstehend.", "Nouvelle session impossible : fermeture en attente.", "Impossibile preparare un'altra sessione: chiusura in corso.", "Geen nieuwe sessie mogelijk: sluiten bezig.", "Nao e possivel preparar outra sessao: fecho pendente."}},
 {{"Preparing player...", "Preparando reproductor...", "Player wird vorbereitet...", "Preparation du lecteur...", "Preparazione lettore...", "Speler voorbereiden...", "A preparar o leitor..."}},
 {{"Stopping and releasing resources...", "Deteniendo y liberando recursos...", "Stoppe und gebe Ressourcen frei...", "Arret et liberation des ressources...", "Arresto e rilascio risorse...", "Stoppen en bronnen vrijgeven...", "A parar e a libertar recursos..."}},
 {{"Error; resources released.", "Error; recursos liberados.", "Fehler; Ressourcen freigegeben.", "Erreur ; ressources liberees.", "Errore; risorse rilasciate.", "Fout; bronnen vrijgegeven.", "Erro; recursos libertados."}},
 {{"Stopped.", "Detenido.", "Gestoppt.", "Arrete.", "Fermato.", "Gestopt.", "Parado."}},
 {{"Close not confirmed. Resources held; Stop retries.", "Cierre sin confirmar. Recursos retenidos; Detener lo reintenta.", "Beenden unbestaetigt. Ressourcen gehalten; Stopp wiederholt.", "Fermeture non confirmee. Ressources retenues ; Arreter reessaie.", "Chiusura non confermata. Risorse trattenute; Stop riprova.", "Sluiten niet bevestigd. Bronnen vastgehouden; Stop probeert opnieuw.", "Fecho nao confirmado. Recursos retidos; Parar tenta de novo."}},
 {{"The CIC refused the close. Resources held; check the logs.", "El CIC rechazo el cierre. Recursos retenidos; revisa los logs.", "CIC hat Beenden abgelehnt. Ressourcen gehalten; Logs pruefen.", "Le CIC a refuse la fermeture. Ressources retenues ; voir les logs.", "Il CIC ha rifiutato la chiusura. Risorse trattenute; vedi i log.", "De CIC weigerde het sluiten. Bronnen vastgehouden; controleer de logs.", "O CIC recusou o fecho. Recursos retidos; ver os logs."}},
 {{"Timeout waiting for CIC confirmation.", "Tiempo agotado esperando confirmacion del CIC.", "Zeitueberschreitung beim Warten auf CIC.", "Delai depasse en attendant le CIC.", "Tempo scaduto in attesa del CIC.", "Time-out bij wachten op CIC.", "Tempo esgotado a aguardar o CIC."}},
 {{"The player ended with an error; check the logs.", "El reproductor termino con error; revisa los logs.", "Player mit Fehler beendet; Logs pruefen.", "Le lecteur s'est arrete sur une erreur ; voir les logs.", "Il lettore e terminato con errore; vedi i log.", "De speler stopte met een fout; zie de logs.", "O leitor terminou com erro; ver os logs."}},
 {{"Timeout preparing the video; closing the session.", "Tiempo agotado preparando el video; cerrando sesion.", "Zeitueberschreitung bei Videostart; Sitzung wird beendet.", "Delai depasse pour la video ; fermeture de la session.", "Tempo scaduto preparando il video; chiusura sessione.", "Time-out bij voorbereiden video; sessie wordt gesloten.", "Tempo esgotado a preparar o video; a fechar sessao."}},
 {{"Player started.", "Reproductor iniciado.", "Player gestartet.", "Lecteur demarre.", "Lettore avviato.", "Speler gestart.", "Leitor iniciado."}},
 {{"Connecting audio...", "Conectando audio...", "Audio wird verbunden...", "Connexion audio...", "Connessione audio...", "Audio verbinden...", "A ligar o audio..."}},
 {{"Opening video...", "Abriendo video...", "Video wird geoeffnet...", "Ouverture de la video...", "Apertura video...", "Video openen...", "A abrir o video..."}},
 {{"Connecting display...", "Conectando pantalla...", "Anzeige wird verbunden...", "Connexion de l'ecran...", "Connessione schermo...", "Scherm verbinden...", "A ligar o ecra..."}},
 {{"Starting video...", "Arrancando video...", "Video wird gestartet...", "Demarrage de la video...", "Avvio video...", "Video starten...", "A iniciar o video..."}},
 {{"Preparing picture...", "Preparando imagen...", "Bild wird vorbereitet...", "Preparation de l'image...", "Preparazione immagine...", "Beeld voorbereiden...", "A preparar a imagem..."}},
 {{"Waiting for the CIC...", "Esperando al CIC...", "Warte auf CIC...", "Attente du CIC...", "In attesa del CIC...", "Wachten op CIC...", "A aguardar o CIC..."}},
 {{"Screen: ", "Pantalla: ", "Bild: ", "Ecran : ", "Schermo: ", "Scherm: ", "Ecra: "}},
 {{"Normal", "Normal", "Normal", "Normal", "Normale", "Normaal", "Normal"}},
 {{"Fullscreen", "Completa", "Vollbild", "Plein ecran", "Schermo intero", "Volledig scherm", "Ecra inteiro"}},
 {{"Stretched", "Estirada", "Gestreckt", "Etire", "Allungato", "Uitgerekt", "Esticado"}},
 {{" | Logs ON", " | Logs ON", " | Logs AN", " | Logs ON", " | Log ON", " | Logs AAN", " | Logs ON"}},
 {{"not started", "sin iniciar", "nicht gestartet", "non demarre", "non avviato", "niet gestart", "nao iniciado"}},
 {{"starting...", "arrancando...", "startet...", "demarrage...", "avvio...", "starten...", "a iniciar..."}},
 {{"error creating thread", "error creando hilo", "Fehler beim Erstellen des Threads", "erreur de creation du thread", "errore creazione thread", "fout bij maken thread", "erro ao criar thread"}},
 {{"AA certificate missing", "Falta certificado AA", "AA-Zertifikat fehlt", "Certificat AA manquant", "Certificato AA mancante", "AA-certificaat ontbreekt", "Falta certificado AA"}},
 {{"hu.crt/hu.key missing (put AAHU.CRT/AAHU.KEY on the USB stick and reinstall)", "falta hu.crt/hu.key (pon AAHU.CRT/AAHU.KEY en el USB y reinstala)", "hu.crt/hu.key fehlt (AAHU.CRT/AAHU.KEY auf USB legen, neu installieren)", "hu.crt/hu.key manquant (AAHU.CRT/AAHU.KEY sur la cle USB, reinstaller)", "manca hu.crt/hu.key (AAHU.CRT/AAHU.KEY sulla chiavetta, reinstallare)", "hu.crt/hu.key ontbreekt (AAHU.CRT/AAHU.KEY op USB, opnieuw installeren)", "falta hu.crt/hu.key (AAHU.CRT/AAHU.KEY na pen, reinstalar)"}},
 {{"Waiting for phone on USB...", "Esperando movil por USB...", "Warte auf Telefon an USB...", "Attente du telephone en USB...", "In attesa del telefono via USB...", "Wachten op telefoon via USB...", "A aguardar telemovel por USB..."}},
 {{"No phone", "Sin movil", "Kein Telefon", "Pas de telephone", "Nessun telefono", "Geen telefoon", "Sem telemovel"}},
 {{"no Android Auto phone detected", "no se detecto movil Android Auto", "kein Android-Auto-Telefon erkannt", "aucun telephone Android Auto detecte", "nessun telefono Android Auto rilevato", "geen Android Auto-telefoon gevonden", "nenhum telemovel Android Auto detetado"}},
 {{"Session ended", "Sesion terminada", "Sitzung beendet", "Session terminee", "Sessione terminata", "Sessie beeindigd", "Sessao terminada"}},
 {{"Session ended with error", "Sesion terminada con error", "Sitzung mit Fehler beendet", "Session terminee avec erreur", "Sessione terminata con errore", "Sessie beeindigd met fout", "Sessao terminada com erro"}},
 {{"AA: session active", "AA: sesion activa", "AA: Sitzung aktiv", "AA : session active", "AA: sessione attiva", "AA: sessie actief", "AA: sessao ativa"}},
 {{"AA: AOA accessory detected", "AA: accesorio AOA detectado", "AA: AOA-Zubehoer erkannt", "AA : accessoire AOA detecte", "AA: accessorio AOA rilevato", "AA: AOA-accessoire gevonden", "AA: acessorio AOA detetado"}},
 {{"AA: phone detected, switching to AOA", "AA: movil detectado, cambiando a AOA", "AA: Telefon erkannt, wechsle zu AOA", "AA : telephone detecte, passage en AOA", "AA: telefono rilevato, passaggio ad AOA", "AA: telefoon gevonden, overschakelen naar AOA", "AA: telemovel detetado, a mudar para AOA"}},
};

static int aap_streq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* English key -> current language. Unknown text is returned unchanged. */
static const char* tr(const char* en) {
    int l = g_lang;
    if (!en || l <= AAP_EN || l >= AAP_LANGS) return en;
    for (unsigned i = 0; i < sizeof kAapTexts / sizeof kAapTexts[0]; ++i)
        if (aap_streq(kAapTexts[i].t[AAP_EN], en)) return kAapTexts[i].t[l];
    return en;
}

/* "stage N/7: ..." (libcicaa result) -> localised prefix + same tail. */
static const char* const kAapStage[AAP_LANGS] = {"stage ", "etapa ", "Stufe ", "etape ", "fase ", "fase ", "etapa "};
static const char* tr_stage(const char* s, char* buf, unsigned cap) {
    const char* k = "stage ";
    unsigned i = 0;
    while (k[i] && s[i] == k[i]) ++i;
    if (k[i]) return tr(s);
    int l = g_lang;
    if (l < 0 || l >= AAP_LANGS) l = AAP_EN;
    unsigned n = 0;
    for (const char* p = kAapStage[l]; *p && n + 1 < cap; ++p) buf[n++] = *p;
    for (const char* p = s + i; *p && n + 1 < cap; ++p) buf[n++] = *p;
    buf[n] = 0;
    return buf;
}
#endif
