
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h" // For esp_rom_delay_us

// --- PIN DEFINITIONS ---
const gpio_num_t colPins[6]      = {GPIO_NUM_16, GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19}; 
const gpio_num_t redRowPins[6]   = {GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23, GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27}; 
const gpio_num_t greenRowPins[6] = {GPIO_NUM_2,  GPIO_NUM_4,  GPIO_NUM_5,  GPIO_NUM_12, GPIO_NUM_32, GPIO_NUM_33}; 

// Buttons (External Pull-ups: 0 = Pressed)
const gpio_num_t btnPins[4] = {GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_39, GPIO_NUM_36}; // 0:TL, 1:TR, 2:BL, 3:BR

// --- DISPLAY BUFFERS ---
volatile bool redMatrix[6][6] = {false};
volatile bool greenMatrix[6][6] = {false};

// --- GAME VARIABLES ---
int currentQuadrant = -1;
int moleCol = 0;
int moleRow = 0;

uint32_t spawnTime = 0;
uint32_t timeLimit = 2000; 
const uint32_t minTimeLimit = 600; 
const uint32_t speedIncrement = 150; 

typedef enum { SPAWN, PLAYING, HIT, GAME_OVER } GameState;
GameState state = SPAWN;

// --- HELPER FUNCTIONS ---

// Replaces Arduino's millis()
uint32_t get_millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Replaces Arduino's random(min, max)
int get_random(int min, int max) {
    return min + (esp_random() % (max - min));
}

void clearMatrix() {
    for (int c = 0; c < 6; c++) {
        for (int r = 0; r < 6; r++) {
            redMatrix[c][r] = false;
            greenMatrix[c][r] = false;
        }
    }
}

void fillRed() {
    for (int c = 0; c < 6; c++) {
        for (int r = 0; r < 6; r++) {
            redMatrix[c][r] = true;
            greenMatrix[c][r] = false;
        }
    }
}

void setMoleColor(bool isGreen) {
    redMatrix[moleCol][moleRow]         = !isGreen;
    redMatrix[moleCol + 1][moleRow]     = !isGreen;
    redMatrix[moleCol][moleRow + 1]     = !isGreen;
    redMatrix[moleCol + 1][moleRow + 1] = !isGreen;

    greenMatrix[moleCol][moleRow]         = isGreen;
    greenMatrix[moleCol + 1][moleRow]     = isGreen;
    greenMatrix[moleCol][moleRow + 1]     = isGreen;
    greenMatrix[moleCol + 1][moleRow + 1] = isGreen;
}

// --- HARDWARE INIT ---
void init_hardware() {
    // Initialize output pins
    for (int i = 0; i < 6; i++) {
        gpio_reset_pin(colPins[i]);
        gpio_set_direction(colPins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(colPins[i], 0);
        
        gpio_reset_pin(redRowPins[i]);
        gpio_set_direction(redRowPins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(redRowPins[i], 0);

        gpio_reset_pin(greenRowPins[i]);
        gpio_set_direction(greenRowPins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(greenRowPins[i], 0);
    }

    // Initialize input pins
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(btnPins[i]);
        gpio_set_direction(btnPins[i], GPIO_MODE_INPUT);
        // We do not enable internal pullups because you have external ones
    }
}

// --- DISPLAY MULTIPLEXING TASK (Core 0) ---
void display_task(void * pvParameters) {
    while(1) {
        for (int col = 0; col < 6; col++) {
            // 1. Turn OFF all columns
            for (int c = 0; c < 6; c++) gpio_set_level(colPins[c], 0); 

            // 2. Set row states
            for (int row = 0; row < 6; row++) {
                gpio_set_level(redRowPins[row], redMatrix[col][row] ? 1 : 0);
                gpio_set_level(greenRowPins[row], greenMatrix[col][row] ? 1 : 0);
            }

            // 3. Turn ON current column
            gpio_set_level(colPins[col], 1); 

            // 4. Microsecond delay for LEDs to shine
            esp_rom_delay_us(1500); 
        }
        // Yield to FreeRTOS scheduler
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

// --- MAIN APPLICATION ENTRY POINT (Core 1) ---
void app_main() {
    init_hardware();

    // Pin the display task to Core 0
    xTaskCreatePinnedToCore(
        display_task,   
        "DisplayTask",  
        4096,           // Stack size in words (not bytes in vanilla FreeRTOS)
        NULL,           
        1,              
        NULL,           
        0               
    );

    // Main Game Loop
    while (1) {
        switch (state) {
            
            case SPAWN:
                clearMatrix();
                
                currentQuadrant = get_random(0, 4); 

                int baseCol = (currentQuadrant == 0 || currentQuadrant == 2) ? 0 : 3;
                int baseRow = (currentQuadrant == 0 || currentQuadrant == 1) ? 3 : 0;

                moleCol = baseCol + get_random(0, 2); 
                moleRow = baseRow + get_random(0, 2);

                setMoleColor(false); 
                
                spawnTime = get_millis();
                state = PLAYING;
                break;

            case PLAYING:
                if (get_millis() - spawnTime > timeLimit) {
                    state = GAME_OVER;
                } else {
                    for (int i = 0; i < 4; i++) {
                        if (gpio_get_level(btnPins[i]) == 0) {
                            
                            if (i == currentQuadrant) {
                                state = HIT;
                            } else {
                                state = GAME_OVER; 
                            }

                            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
                            // Wait for release
                            while (gpio_get_level(btnPins[i]) == 0) {
                                vTaskDelay(pdMS_TO_TICKS(10));
                            }
                            break; 
                        }
                    }
                }
                // Yield to prevent watchdog triggers in the while loop
                vTaskDelay(pdMS_TO_TICKS(10));
                break;

            case HIT:
                setMoleColor(true); 
                
                if (timeLimit > minTimeLimit + speedIncrement) {
                    timeLimit -= speedIncrement; 
                } else {
                    timeLimit = minTimeLimit; 
                }

                vTaskDelay(pdMS_TO_TICKS(400)); 
                state = SPAWN;
                break;

            case GAME_OVER:
                fillRed();
                vTaskDelay(pdMS_TO_TICKS(3000)); 
                timeLimit = 2000; 
                state = SPAWN;
                break;
        }
    }
}