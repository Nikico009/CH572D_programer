#include "CH57x_common.h"

#define LED_PIN GPIO_Pin_2


int main(void) {
    // Set CH572D to work with external 32MHz crystal
    SetSysClock(CLK_SOURCE_HSE_PLL_60MHz);

    // Configure LED_PIN to work as an output with DS of 5mA
    GPIOA_ModeCfg(LED_PIN, GPIO_ModeOut_PP_5mA);

    while(1) {
        GPIOA_InverseBits(LED_PIN); // Invert LED state
        mDelaymS(500);
    }
}