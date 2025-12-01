/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "sdkconfig.h"
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "spi_flash_mmap.h"
#include "esp_flash.h"
#include "protocol_examples_common.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <esp_system.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <driver/uart.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/mcpwm_prelude.h>
#include "driver/gpio.h"
#include "bdc_motor.h"
#include "esp_timer.h"
#include <math.h>

static const char* TAG = "main";

/* FlaskサーバのホストIPとポート番号 */
#define WEB_SERVER "10.0.0.85"
#define WEB_PORT   "50000"

/* ADC値が 3000 以上で「食事開始」とみなす */
/* ADC値が 2000 以下で「食事終了」とみなす */
#define ADC_THRESHOLD_1 2000
#define ADC_THRESHOLD_2 3000
/* ========= グローバル変数 ========== */
static int adValue = 0;                // 最新のADC読み取り値
static bool meal_in_progress = false;  // 食事中かどうかを示すフラグ

/* ========== シンプルなHTTP送信関数 =========== */
static esp_err_t send_http_msg(const char *msg)
{
    /*
       msg に "mealStart" や "mealEnd" を渡すと、以下のようなHTTPリクエストを送信する:
           GET /mealStart HTTP/1.0
           Host: 192.168.12.179:50000
           ...
    */
    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    char request[128];
    int s, r;

    // HTTPリクエストのヘッダおよびHost情報を組み立てる
    snprintf(request, sizeof(request),
             "GET /%s HTTP/1.0\r\n"
             "Host: %s:%s\r\n"
             "User-Agent: esp32\r\n"
             "\r\n",
             msg, WEB_SERVER, WEB_PORT);

    // サーバアドレスを解決
    int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        if (res) {
            freeaddrinfo(res);
        }
        return ESP_FAIL;
    }

    // ソケット作成
    s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket.");
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    // サーバに接続
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    freeaddrinfo(res);

    // HTTPリクエスト送信
    if (write(s, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket send failed");
        close(s);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, ">>> Socket send success: %s", request);

    // 受信タイムアウト設定 (5秒)
    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout));

    // HTTPレスポンスを読む（任意）
    char recv_buf[64];
    do {
        bzero(recv_buf, sizeof(recv_buf));
        r = read(s, recv_buf, sizeof(recv_buf)-1);
        if (r > 0) {
            recv_buf[r] = 0; // 終端文字
            ESP_LOGI(TAG, ">>> Recv: %s", recv_buf);
        }
    } while(r > 0);

    // ソケットを閉じる
    close(s);
    return ESP_OK;
}

/* ========== PWM と ADC に関連する設定 ========== */
#define BDC_MCPWM_TIMER_RESOLUTION_HZ 10000000 // 10MHz, 1tick = 0.1us
#define BDC_MCPWM_FREQ_HZ             25000    // 25KHz
#define BDC_MCPWM_DUTY_TICK_MAX       (BDC_MCPWM_TIMER_RESOLUTION_HZ / BDC_MCPWM_FREQ_HZ)

static adc_oneshot_unit_handle_t adc1_handle;
bdc_motor_handle_t motor = NULL;

/* ダンピング係数や周波数などの複数プリセットを持つ構造体 */
struct WaveParam {
    const double damp[3];
    const int nDamp;
    const double freq[6];
    const int nFreq;
    const double amplitude;
} wave = {
    .damp = {-2, -5, -10},
    .nDamp = 3,
    .freq = {10, 20, 50, 100, 200, 500},
    .nFreq = 6,
    .amplitude = 2,
};

static int count = 0;
static double since = -1;  // 振動開始からの経過時間を管理

#define USE_TIMER  // タイマーを使って周期実行する場合はこのマクロを定義

/* ========== 食事状態を判定してサーバへ送信するロジック ========== */
static void checkMealState(int adc_val)
{
    // 現在「食事中でない」かつ ADC値 >= 閾値 -> 食事開始を送信
    if (!meal_in_progress && adc_val >= ADC_THRESHOLD_2) {
        meal_in_progress = true;
        ESP_LOGI(TAG, ">>> Detected mealStart");
        send_http_msg("mealStart");
    }
    // 現在「食事中」かつ ADC値 < 閾値 -> 食事終了を送信
    else if (meal_in_progress && adc_val < ADC_THRESHOLD_1) {
        meal_in_progress = false;
        ESP_LOGI(TAG, ">>> Detected mealEnd");
        send_http_msg("mealEnd");
    }
}

/* ========== 振動を制御するコア関数 (タイマーやタスクから呼ばれる) ========== */
void hapticFunc(void* arg)
{
    static int i = 0;
    static double omega = 0;
    static double B = 0;

    // ADC値を読み込む
    adc_oneshot_read(adc1_handle, ADC_CHANNEL_6, &adValue);

    // 食事状態のチェック -> 必要なら mealStart / mealEnd を送信
    checkMealState(adValue);

    // 以下は元々の振動モータ用の波形制御サンプル
    if (adValue < 2100 && since > 0.3) {
        since = -1;
        printf("\r\n");
    }
    if (adValue > 2400 && since == -1){
        // 新しいパラメータ（周波数など）を選択
        since = 0;
        omega = wave.freq[i % wave.nFreq] * M_PI * 2;
        B = wave.damp[i / wave.nFreq];
        printf("Wave: %3.1fHz, A=%2.2f, B=%3.1f ", omega/(M_PI*2), wave.amplitude, B);
        i++;
        if (i >= wave.nFreq * wave.nDamp) i = 0;
    }

    // 波形の出力計算
    double pwm = 0;
    if (since >= 0) {
        pwm = wave.amplitude * cos(omega * since) * exp(B * since);
#ifdef USE_TIMER
        // タイマー割り込みの場合、周期が 100us なら
        since += 0.0001;
#else
        // タスクの場合、周期が 1ms なら
        // since += 0.001;
#endif
    }

    // 回転方向を判定
    if (pwm > 0) {
        bdc_motor_forward(motor);
    } else {
        bdc_motor_reverse(motor);
        pwm = -pwm;
    }
    if (pwm > 1) pwm = 1;

    // PWMデューティを設定
    unsigned int speed = pwm * BDC_MCPWM_DUTY_TICK_MAX;
    bdc_motor_set_speed(motor, speed);

    // 定期的にADC値を出力（例: 1000回ごとに1回ログ）
    count++;
    if (count >= 1000) {
        ESP_LOGI(TAG, "ADC:%d", adValue);
        count = 0;
    }
}

#ifndef USE_TIMER
// タスクを使って周期的に hapticFunc を呼ぶ場合
void hapticTask(void* arg)
{
    while(1){
        hapticFunc(arg);
        vTaskDelay(pdMS_TO_TICKS(1)); // 1ms周期
    }
}
#endif

void app_main(void)
{
    /* チップ情報を表示 */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s)...\n", CONFIG_IDF_TARGET, chip_info.cores);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed\n");
        return;
    }

    /* NVSやネットワーク(Wi-Fi)関連の初期化 */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect()); // Wi-Fi接続

    // ADC の初期化
    ESP_LOGI(TAG, "Initialize ADC");
    adc_oneshot_unit_init_cfg_t adc_init_config1 = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t adc1_chan6_cfg = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_6, &adc1_chan6_cfg));

    // GPIO 初期化
    ESP_LOGI(TAG, "Initialize GPIO");
    gpio_config_t gpio_conf = {
        .pin_bit_mask = 1ULL << 16,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_conf);
    gpio_set_level(GPIO_NUM_16, 1);

    // DCモータ用の PWM 初期化
    ESP_LOGI(TAG, "Initialize PWM for DC motor");
    bdc_motor_config_t motor_config = {
        .pwma_gpio_num = GPIO_NUM_5,
        .pwmb_gpio_num = GPIO_NUM_17,
        .pwm_freq_hz   = BDC_MCPWM_FREQ_HZ,
    };
    bdc_motor_mcpwm_config_t mcpwm_config = {
        .group_id = 0,
        .resolution_hz = BDC_MCPWM_TIMER_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(bdc_motor_new_mcpwm_device(&motor_config, &mcpwm_config, &motor));
    ESP_ERROR_CHECK(bdc_motor_enable(motor));

#ifdef USE_TIMER
    // ハードウェアタイマーを使って周期実行
    esp_timer_create_args_t timerDesc = {
        .callback = hapticFunc,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haptic",
        .skip_unhandled_events = true
    };
    esp_timer_handle_t timerHandle = NULL;
    esp_timer_create(&timerDesc, &timerHandle);
    // 100us (0.0001s) ごとに hapticFunc を呼ぶ
    esp_timer_start_periodic(timerHandle, 100); // マイクロ秒単位
#else
    // FreeRTOS タスクで周期実行
    TaskHandle_t taskHandle = NULL;
    xTaskCreate(hapticTask, "hapticTask", 4096, NULL, 5, &taskHandle);
#endif

    // UARTの受信などを行うループ
    uart_driver_install(UART_NUM_0, 1024, 1024, 10, NULL, 0);
    while(1){
        uint8_t ch;
        uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY);
        printf("'%c' received.\r\n", ch);
        // シリアルからの入力に応じた処理
        switch(ch){
            case 'a':
                // 必要に応じて処理
                break;
            default:
                break;
        }
    }
}


