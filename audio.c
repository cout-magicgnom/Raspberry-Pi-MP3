#define MINIAUDIO_IMPLEMENTATION
#include "headers/miniaudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "headers/estados.h"


#ifdef _WIN32
    #include <windows.h>
    #include <conio.h>

    /* ---- Nomes usados no resto do arquivo, unificados por macro ---- */
    #define STRCASECMP _stricmp
    #define STRDUP     _strdup

    void sleep_ms(int ms) {
        Sleep(ms);
    }

    void clear_console(void) {
        system("cls");
    }

    /* Wrappers finos em cima do conio.h: no Windows, kbhit()/getch()
       ja fazem exatamente o que precisamos (nao-bloqueante e leitura
       crua de tecla), entao so damos nomes portaveis a eles. */
    static int os_kbhit(void) { return _kbhit(); }
    static int os_getch(void) { return _getch(); }

#else
    #include <time.h>
    #include <unistd.h>
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/select.h>
    #include <termios.h>
    #include <strings.h>
    #include <limits.h>

    /* ---- Equivalentes POSIX das funcoes/macros especificas do MSVC ---- */
    #define STRCASECMP strcasecmp
    #define STRDUP     strdup

    /* MAX_PATH nao existe fora do Windows; usamos o equivalente do sistema. */
    #ifndef MAX_PATH
        #define MAX_PATH PATH_MAX
    #endif

    void sleep_ms(int ms) {
        struct timespec ts;

        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;

        nanosleep(&ts, NULL);
    }

    void clear_console(void) {
        system("clear");
    }

    static struct termios term_original;
    static bool           term_configurado = false;

    static void restaurarTerminal(void){
        if (term_configurado){
            tcsetattr(STDIN_FILENO, TCSANOW, &term_original);
        }
    }

    static void configurarTerminalRaw(void){
        if (term_configurado) return;

        tcgetattr(STDIN_FILENO, &term_original);

        struct termios raw = term_original;
        raw.c_lflag &= ~(ICANON | ECHO);   // sem buffer de linha, sem eco
        raw.c_iflag &= ~(ICRNL);           // Enter chega como '\r' (13), igual ao Windows
        raw.c_cc[VMIN]  = 0;               // leitura nao-bloqueante...
        raw.c_cc[VTIME] = 0;               // ...retorna na hora mesmo sem tecla

        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        term_configurado = true;
        atexit(restaurarTerminal);         // garante terminal normal ao fechar
    }

    /* Retorna 1 se ha uma tecla esperando para ser lida, sem consumi-la. */
    static int os_kbhit(void){
        configurarTerminalRaw();

        fd_set fds;
        struct timeval tv = {0, 0};
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
    }

    /* Le um unico byte cru do teclado (equivalente ao _getch() do Windows). */
    static int os_getch(void){
        configurarTerminalRaw();

        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
        return -1;
    }

#endif


/* ============================================================
   CONFIGURACAO MODULAR DOS BOTOES DE ACAO
   ============================================================ */
#define TECLA_AUTOPLAY 'A'
#define TECLA_PAUSE    'P'

/* Teclas de navegacao*/
#define TECLA_SETA_CIMA   72
#define TECLA_SETA_BAIXO  80
#define TECLA_ENTER       13
#define TECLA_ESC         27

/* Pasta onde ficam os arquivos de musica da radio. */
#define DIRETORIO_MUSICAS "musicas/"

typedef struct {
    char *titulo;   // alocado dinamicamente, gerado a partir do nome do arquivo
    char *arquivo;  // caminho completo do arquivo, alocado dinamicamente
} Musica;

static ma_engine engine;
static ma_sound  somAtual;
static bool      somCarregado  = false;
static int       indiceAtual   = -1;
static bool      autoPlayAtivo = true;
static bool      pausado       = false;

//! Playlist dinamica de musicas carregadas do diretorio DIRETORIO_MUSICAS

static Musica *radioPlaylist   = NULL;
static int     totalMusicas    = 0;  // quantidade de musicas carregadas
static int     capacidadeLista = 0;  // capacidade atual do vetor alocado

int audioInit(){
    return ma_engine_init(NULL, &engine);
}

/* ============================================================
   BIBLIOTECA DINAMICA DE MUSICAS (varredura de diretorio + realloc)
   ============================================================ */

static void garantirCapacidade(){
    if (totalMusicas < capacidadeLista) return;

    int novaCapacidade = (capacidadeLista == 0) ? 4 : capacidadeLista * 2;
    Musica *novo = realloc(radioPlaylist, novaCapacidade * sizeof(Musica));

    if (novo == NULL){
        printf("Erro: sem memoria para carregar mais musicas.\n");
        return;
    }

    radioPlaylist   = novo;
    capacidadeLista = novaCapacidade;
}

//! Verifica se o arquivo tem extensao de audio suportada

static bool extensaoValida(const char *nome){
    const char *ponto = strrchr(nome, '.');
    if (!ponto) return false;

    return (STRCASECMP(ponto, ".mp3")  == 0 ||
            STRCASECMP(ponto, ".wav")  == 0 ||
            STRCASECMP(ponto, ".ogg")  == 0 ||
            STRCASECMP(ponto, ".flac") == 0);
}

//! Gera um titulo 
static char *gerarTitulo(const char *nomeArquivo){
    const char *ponto = strrchr(nomeArquivo, '.');
    size_t tamanho = ponto ? (size_t)(ponto - nomeArquivo) : strlen(nomeArquivo);

    char *titulo = malloc(tamanho + 1);
    if (!titulo) return NULL;

    for (size_t i = 0; i < tamanho; i++){
        char c = nomeArquivo[i];
        titulo[i] = (c == '-' || c == '_') ? ' ' : c;
    }
    titulo[tamanho] = '\0';

    return titulo;
}

//! Adiciona uma musica na playlist dinamica, alocando memoria propria
//! para o titulo e para o caminho do arquivo.
static void adicionarMusica(const char *caminhoCompleto, const char *nomeArquivo){
    garantirCapacidade();
    if (totalMusicas >= capacidadeLista) return; // realloc falhou, aborta esta musica

    char *titulo  = gerarTitulo(nomeArquivo);
    char *arquivo = STRDUP(caminhoCompleto);

    if (!titulo || !arquivo){
        free(titulo);
        free(arquivo);
        return;
    }

    radioPlaylist[totalMusicas].titulo  = titulo;
    radioPlaylist[totalMusicas].arquivo = arquivo;
    totalMusicas++;
}

//! Varre DIRETORIO_MUSICAS e monta a playlist dinamicamente, uma musica
//! por arquivo de audio encontrado.
#ifdef _WIN32

static void carregarPlaylistDoDiretorio(){
    char padraoBusca[MAX_PATH];
    snprintf(padraoBusca, sizeof(padraoBusca), "%s*.*", DIRETORIO_MUSICAS);

    WIN32_FIND_DATA dado;
    HANDLE busca = FindFirstFile(padraoBusca, &dado);

    if (busca == INVALID_HANDLE_VALUE){
        printf("Aviso: nao foi possivel abrir o diretorio de musicas: %s\n", DIRETORIO_MUSICAS);
        return;
    }

    do { //! do while para percorrer des do primeiro arquivo encontrado
        if (dado.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!extensaoValida(dado.cFileName)) continue;

        char caminhoCompleto[MAX_PATH];
        snprintf(caminhoCompleto, sizeof(caminhoCompleto), "%s%s", DIRETORIO_MUSICAS, dado.cFileName);

        adicionarMusica(caminhoCompleto, dado.cFileName);

    } while (FindNextFile(busca, &dado));

    FindClose(busca);
}

#else

/* Equivalente Linux/Mac da varredura acima: FindFirstFile/FindNextFile
   sao exclusivos da Win32 API, entao usamos opendir/readdir (POSIX),
   com stat() para pular subdiretorios (o WIN32_FIND_DATA fazia isso
   checando FILE_ATTRIBUTE_DIRECTORY; aqui o equivalente e S_ISDIR). */
static void carregarPlaylistDoDiretorio(){
    DIR *dir = opendir(DIRETORIO_MUSICAS);

    if (dir == NULL){
        printf("Aviso: nao foi possivel abrir o diretorio de musicas: %s\n", DIRETORIO_MUSICAS);
        return;
    }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL){
        if (strcmp(entrada->d_name, ".") == 0 || strcmp(entrada->d_name, "..") == 0) continue;

        char caminhoCompleto[MAX_PATH];
        snprintf(caminhoCompleto, sizeof(caminhoCompleto), "%s%s", DIRETORIO_MUSICAS, entrada->d_name);

        struct stat info;
        if (stat(caminhoCompleto, &info) == 0 && S_ISDIR(info.st_mode)) continue;

        if (!extensaoValida(entrada->d_name)) continue;

        adicionarMusica(caminhoCompleto, entrada->d_name);
    }

    closedir(dir);
}

#endif

/* Libera toda a memoria alocada dinamicamente para a playlist.
   Chamada no encerramento do audio (audioClose). */
static void liberarPlaylist(){
    for (int i = 0; i < totalMusicas; i++){
        free(radioPlaylist[i].titulo);
        free(radioPlaylist[i].arquivo);
    }
    free(radioPlaylist);
    radioPlaylist   = NULL;
    totalMusicas    = 0;
    capacidadeLista = 0;
}

/* ============================================================
   CONTROLE DE REPRODUCAO
   ============================================================ */

static void pararSomAtual(){
    if (somCarregado){
        ma_sound_stop(&somAtual);
        ma_sound_uninit(&somAtual);
        somCarregado = false;
        pausado      = false;
    }
}

void audioClose(){
    pararSomAtual();
    liberarPlaylist();
    ma_engine_uninit(&engine);
}

/* Toca uma musica da playlist. SEMPRE para a anterior antes de comecar
   a proxima, evitando que o audio fique sobreposto. */
static void tocarMusica(int indice){
    if (indice < 0 || indice >= totalMusicas) return;

    pararSomAtual();

    if (ma_sound_init_from_file(&engine, radioPlaylist[indice].arquivo,
                                 0, NULL, NULL, &somAtual) != MA_SUCCESS){
        printf("Erro ao carregar: %s\n", radioPlaylist[indice].arquivo);
        return;
    }

    ma_sound_start(&somAtual);
    somCarregado = true;
    indiceAtual  = indice;
    pausado      = false; //! toda musica nova comeca tocando, nunca pausada
}

/* Alterna entre pausar e retomar a musica atual (mantendo a posicao,
   diferente de pararSomAtual, que descarta o som por completo). */
static void alternarPause(){
    if (!somCarregado) return;

    if (!pausado){
        ma_sound_stop(&somAtual);
        pausado = true;
    } else {
        ma_sound_start(&somAtual);
        pausado = false;
    }
}

/* Mantida por compatibilidade com o resto do projeto - agora tambem
   para qualquer som anterior antes de iniciar (evita sobreposicao). */
void playBackground(const char *arquivo){
    pararSomAtual();

    if (ma_sound_init_from_file(&engine, arquivo, 0, NULL, NULL, &somAtual) != MA_SUCCESS){
        return;
    }

    ma_sound_set_looping(&somAtual, MA_TRUE);
    ma_sound_start(&somAtual);
    somCarregado = true;
}

/* Mantida por compatibilidade - dispara um som "solto" pelo engine
   (nao e usada pelo radio, que usa tocarMusica para controlar sobreposicao) */
void playAudio(const char *arquivo){
    ma_engine_play_sound(&engine, arquivo, NULL);
}

//! Leitura de tecla com suporte a setas e ESC, independente do SO.
#ifdef _WIN32

static int lerTecla(void){
    int tecla = os_getch();

    if (tecla == 0 || tecla == 224){
        tecla = os_getch(); // segundo byte = codigo real da tecla especial
    }

    return tecla;
}

#else

static int lerTecla(void){
    int c = os_getch();

    if (c == 27){
        /* Pode ser um ESC "puro" (usuario apertou ESC) ou o inicio de
           uma sequencia de seta. Da uma pequena chance dos proximos
           bytes chegarem antes de decidir. */
        if (os_kbhit()){
            int c2 = os_getch();
            if (c2 == '[' && os_kbhit()){
                int c3 = os_getch();
                if (c3 == 'A') return TECLA_SETA_CIMA;
                if (c3 == 'B') return TECLA_SETA_BAIXO;
            }
        }
        return TECLA_ESC;
    }

    return c;
}

#endif

/* ============================================================
   INTERFACE COM BOTOES 
   ============================================================ */

static void desenharTela(int selecionado, int botaoAutoplay, int botaoPause, int botaoDesligar){
    clear_console();

    if (totalMusicas == 0){
        printf(" Nenhuma musica encontrada em \"%s\".\n\n", DIRETORIO_MUSICAS);
    }

    for (int i = 0; i < totalMusicas; i++){
        printf("%s [ %-20s ]", (i == selecionado) ? " >" : "  ", radioPlaylist[i].titulo);

        if (i == indiceAtual && somCarregado)
            printf("  <-- tocando agora\n");
        else
            printf("\n");
    }

    printf("%s [ PLAY AUTOMATICO: %s ]  (atalho: tecla '%c')\n",
           (selecionado == botaoAutoplay) ? " >" : "  ",
           autoPlayAtivo ? "LIGADO" : "DESLIGADO",
           TECLA_AUTOPLAY);

    printf("%s [ %s ]  (atalho: tecla '%c')\n",
           (selecionado == botaoPause) ? " >" : "  ",
           pausado ? "RETOMAR" : "PAUSAR",
           TECLA_PAUSE);

    printf("%s [ DESLIGAR ]\n", (selecionado == botaoDesligar) ? " >" : "  ");

    printf("\nSetas CIMA/BAIXO para navegar, ENTER para selecionar, ESC para voltar.\n");
}

Estado radio(){
    //! Carrega a playlist a partir do diretorio apenas uma vez,
    //! na primeira vez que a radio e aberta. 
    static bool playlistCarregada = false;
    if (!playlistCarregada){
        carregarPlaylistDoDiretorio();
        playlistCarregada = true;
    }

    const int botaoAutoplay  = totalMusicas;
    const int botaoPause     = totalMusicas + 1;
    const int botaoDesligar  = totalMusicas + 2;
    const int totalBotoes    = totalMusicas + 3;

    int selecionado = 0;

    desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);

    while (1){

        /* Se o autoplay estiver ligado, a musica atual tiver terminado e
           nao estiver pausada, avanca automaticamente para a proxima. */
        if (autoPlayAtivo && somCarregado && !pausado && ma_sound_at_end(&somAtual)){
            int proximo = (indiceAtual + 1) % totalMusicas;
            tocarMusica(proximo);
            desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
        }

        if (os_kbhit()){
            int tecla = lerTecla();

            if (tecla == TECLA_SETA_CIMA){
                selecionado = (selecionado - 1 + totalMusicas) % totalMusicas;
                tocarMusica(selecionado);
                desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
            }
            else if (tecla == TECLA_SETA_BAIXO){
                selecionado = (selecionado + 1) % totalMusicas;
                tocarMusica(selecionado);
                desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
            }
            else if (tecla == TECLA_ENTER){
                if (selecionado < totalMusicas) {
                    tocarMusica(selecionado);
                }
                else if (selecionado == botaoAutoplay){
                    autoPlayAtivo = !autoPlayAtivo;
                }

                else if (selecionado == botaoDesligar){
                    return MENU; // Altere para o estado correto, se necessario
                }
                desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
            }
            else if (tecla == TECLA_AUTOPLAY || tecla == (TECLA_AUTOPLAY + 32)){
                //! Atalho direto: para mudar a tecla, basta alterar o #define 
                //! TECLA_AUTOPLAY no topo do arquivo.
                autoPlayAtivo = !autoPlayAtivo;
                desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
            }
            else if (tecla == TECLA_PAUSE || tecla == (TECLA_PAUSE + 32)){
                //! Mesma logica do atalho de autoplay: funciona de
                //! qualquer lugar do menu e a tecla e trocavel via
                //! #define TECLA_PAUSE.
                alternarPause();
                desenharTela(selecionado, botaoAutoplay, botaoPause, botaoDesligar);
            }
            else if (tecla == TECLA_ESC){
                return MENU;
            }
        }

        sleep_ms(50); // evita uso excessivo de CPU
    }
}

