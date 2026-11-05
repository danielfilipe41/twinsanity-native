// The port's own menu lines (porttext.h): english, then the four other languages the disc carries, in the slots the texts are read
// from (ConsoleLanguage: 1 english, 2 french, 3 spanish, 4 german, 5 italian).
#include "game/porttext.h"

#include "game/language.h"

#include "platform/files.h"

namespace
{
// A slot per language the disc carries, the numbers the text files' slots have
constexpr u32 Slots = 7;
constexpr u32 FirstLine = PortTextFirst;

// The columns: the slots the texts are read in (english 0, french 1, german 2, spanish 3, italian 4) and japanese, which is not a
// slot of its own: the Japanese release's texts fill the english slot (files.cpp's alias)
constexpr u32 JapaneseColumn = 5;

const char* const Lines[PortTextCount - PortTextFirst][Slots] = {
    {"resolution", "résolution", "auflösung", "resolución", "risoluzione", "解像度"},
    {"window", "fenêtre", "fenster", "ventana", "finestra", "ウィンドウモード"},
    {"frame rate", "fréquence d'images", "bildrate", "fotogramas", "frequenza dei fotogrammi", "フレームレート"},
    {"aspect", "format", "format", "formato", "formato", "アスペクト比"},
    {"render scale", "échelle de rendu", "renderskalierung", "escala de renderizado", "scala di rendering", "描画スケール"},
    {"antialiasing", "anticrénelage", "kantenglättung", "suavizado de bordes", "antialiasing", "アンチエイリアス"},
    {"pop-in distance", "distance d'apparition", "sichtweite", "distancia de aparición", "distanza di comparsa", "描画距離"},
    {"voices (restart)", "voix (redémarrage)", "stimmen (neustart)", "voces (reinicio)", "voci (riavvio)",
     "音声（要再起動）"},
    {"texts (restart)", "textes (redémarrage)", "texte (neustart)", "textos (reinicio)", "testi (riavvio)",
     "テキスト（要再起動）"},
    {"exit", "quitter", "beenden", "salir", "esci", "終了"},
    {"close game", "fermer le jeu", "spiel beenden", "cerrar el juego", "chiudi il gioco", "ゲームを終了"},
    {"do you really want to terminate the game?", "voulez-vous vraiment fermer le jeu ?",
     "wollen sie das spiel wirklich beenden?", "¿seguro que quieres cerrar el juego?", "vuoi davvero chiudere il gioco?",
     "本当にゲームを終了しますか？"},
    // The save screens, said the way a computer's saves are (the console's memory card wording is retail's)
    {"checking save files", "vérification des fichiers de sauvegarde", "save-dateien werden geprüft",
     "comprobando archivos de guardado", "controllo dei file di salvataggio", "セーブファイルを確認中"},
    {"creating save file folder failed", "la création du dossier de sauvegarde a échoué",
     "ordner für spielstände konnte nicht erstellt werden", "no se pudo crear la carpeta de guardado",
     "creazione della cartella di salvataggio non riuscita", "セーブフォルダの作成に失敗しました"},
    {"loading save files from slots", "chargement des fichiers de sauvegarde", "spielstände werden geladen",
     "cargando archivos de guardado", "caricamento dei file di salvataggio", "セーブファイルを読み込み中"},
    {"no save file created", "aucun fichier de sauvegarde créé", "kein spielstand erstellt",
     "no se creó ningún archivo de guardado", "nessun file di salvataggio creato", "セーブファイルは作成されていません"},
    {"are you sure?", "êtes-vous sûr ?", "sind sie sicher?", "¿estás seguro?", "sei sicuro?", "よろしいですか？"},
    {"controller options", "options de la manette", "controller-optionen", "opciones del mando", "opzioni del controller",
     "コントローラー設定"},
    {"press a key or a button", "appuyez sur une touche ou un bouton", "taste oder knopf drücken",
     "pulsa una tecla o un botón", "premi un tasto o un pulsante", "キーかボタンを押してください"},
};
}

const char* PortTextString(u32 text)
{
    if (text < FirstLine || text >= PortTextCount)
    {
        // Not one of the port's: the disc's own table answers it (and only that table is ever asked for these)
        return "";
    }

    const u32 line = text - FirstLine;
    u32 slot = g_CurrentTextLanguage;
    if (slot >= JapaneseColumn)
    {
        slot = 0;
    }

    // The Japanese release's texts are its own language whatever slot they were read from (its english one, which the file layer
    // fills with the Japanese files); every other disc's texts are the slot's
    if (Platform::Files::DiscIsJapanese())
    {
        slot = JapaneseColumn;
    }

    const char* name = Lines[line][slot];
    // A line a language has not been given: english is the one every line has
    return name != nullptr ? name : Lines[line][0];
}
