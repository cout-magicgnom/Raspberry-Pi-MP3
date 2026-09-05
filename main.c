#include <stdio.h>
#include "headers/estados.h"
#include "headers/estados.h"
#include "headers/audio.h"

int main(){

    if (audioInit() != 0){
        printf("Erro ao inicializar o sistema de audio.\n");
        return 1;
    } 

    Estado estadoAtual = RADIO;

    while (1){

        switch (estadoAtual){

            case RADIO:
                return radio();
                break;

            default:
                printf("Estado desconhecido. Encerrando.\n");
                estadoAtual = DESLIGAR;
                break;
        }
    }

    audioClose();

    return 0;
}
