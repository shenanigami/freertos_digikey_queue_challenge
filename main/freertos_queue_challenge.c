#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "[QUEUE] ";

// use only 1 core
// enable CONFIG_FREERTOS_UNICORE

// uart driver configuration
#define ECHO_TEST_TXD GPIO_NUM_1
#define ECHO_TEST_RXD GPIO_NUM_3
// no flow control
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      UART_NUM_0
#define ECHO_UART_BAUD_RATE     115200

#define DELAY_TASK_STACK_SIZE    2*1024
#define BLINK_TASK_STACK_SIZE    2*1024

// buffer sizes
#define BUF_SIZE 256
#define DATA_BUF_SIZE 2
#define DELAY_BUF_SIZE 6
#define BLINK_BUF_SIZE 32
#define BLINK_COUNT_INTERVAL 10  // times

// led defines
#define BLINK_GPIO 13

// Task Handles
static TaskHandle_t x_delay_task = NULL, x_blink_task = NULL;

// Queue handles
static QueueHandle_t queue1;
static QueueHandle_t queue2;
static const uint8_t queue_len = 10;
static const uint8_t queue_len_near_full = 8;

static void delay_read_timer_callback(void* arg) {

	ESP_LOGI(TAG, "Delay timeout");
}

static bool readDelayNum(uint32_t *delay_num_buffer, esp_timer_handle_t timer) {

	uint32_t delay_num_temp_buffer = 0;
	bool is_delay_num = true;
	uint8_t c = 0;
	int len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	while (c != ' ' && c != '\t' && c != '\r' && delay_num_temp_buffer < UINT32_MAX) {
		if (len > 0 && c > 0) {
			uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) &c, len);
			if ((c > '9' || c < '0') || (delay_num_temp_buffer > delay_num_temp_buffer*10 + (c - '0'))) {
				is_delay_num = false;
				break;
			}

			delay_num_temp_buffer *= 10;
			delay_num_temp_buffer += (c - '0');
		} else if (!esp_timer_is_active(timer)) {
			is_delay_num = false;
			break;
		}

		len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	}

	if (is_delay_num)
		*delay_num_buffer = delay_num_temp_buffer;
	return is_delay_num;
}

static bool readUntilWhitespaceOrDelay(size_t cmd_word_size, const char* cmd_word, uint32_t* delay_num_buffer)
{
	bool is_cmd_word = true;
	uint8_t c = 0;
	int i = 1;


	const esp_timer_create_args_t delay_read_timer_args = {
		.callback = &delay_read_timer_callback,
		/* name is optional, but may help identify the timer when debugging */
		.name = "delay_read"
	};

	esp_timer_handle_t delay_read_timer;
	ESP_ERROR_CHECK(esp_timer_create(&delay_read_timer_args, &delay_read_timer));
	/* The timer has been created but is not running yet */

	// *delay_num_buffer*2*BLINK_COUNT_INTERVAL -> 1 Blinked queue entry in ms
	// queue_len_near_full is defined as const global
	ESP_ERROR_CHECK(esp_timer_start_once(delay_read_timer, (*delay_num_buffer*2*BLINK_COUNT_INTERVAL*queue_len_near_full)*1000));

	int len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	while (i < cmd_word_size) {

		if (len > 0 && c > 0) {
			uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) &c, len);
			if (c == 'd') {
				i = 0;
				ESP_LOGD(TAG, "Delay soon?");
			} else if (c != cmd_word[i]) {
				is_cmd_word = false;
				break;
			}
			i++;
		} else if (!esp_timer_is_active(delay_read_timer)) {
			is_cmd_word = false;
			break;
		}
		len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	}

	bool is_delay_num = is_cmd_word ? readDelayNum(delay_num_buffer, delay_read_timer) : false;

	// Only running timers can be stopped
	if (esp_timer_is_active(delay_read_timer))
		ESP_ERROR_CHECK(esp_timer_stop(delay_read_timer));
	// only stopped/inactive timers can be deleted
	ESP_ERROR_CHECK(esp_timer_delete(delay_read_timer));

	return is_cmd_word && is_delay_num;
}

static void delay_task(void *arg)
{

	// Configure a temporary buffer for the incoming data
	uint8_t data[DATA_BUF_SIZE] = { 0 };
	bool is_delay = false;
	static uint32_t delay_num_buffer = 500;
	const char* delay_cmd = "delay ";
	char queue2_buffer[BLINK_BUF_SIZE] = { 0 };

	while (1) {
		if (ulTaskNotifyTake(pdTRUE, 0) == pdPASS) {
			while (xQueueReceive(queue2, (void *) queue2_buffer, 0) == pdTRUE)
				ESP_LOGI(TAG, "in %s: %s", __func__, queue2_buffer);
		}
		// Read data from the UART
		int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, 1, 20 / portTICK_PERIOD_MS);
		if (*data == '\r')
			*data = '\n';
		else if (*data == 'd') {
			ESP_LOGD(TAG, "Delay, is that you?");
			is_delay = true;
		}

		// Write data back to the UART
		uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) data, len);
		if (len) {
			data[len] = '\0';
			ESP_LOGD(TAG, "Recv str: %s", (char *) data);
		}

		if (is_delay) {
			if (readUntilWhitespaceOrDelay(DELAY_BUF_SIZE, delay_cmd, &delay_num_buffer)) {
				ESP_LOGI(TAG, "delay %lu", delay_num_buffer);
				xQueueSend(queue1, (void *) &delay_num_buffer, 10);

				// Send notification to blink_task
				xTaskNotify(x_blink_task, 0, eIncrement);
				if (eBlocked == eTaskGetState(x_blink_task))
					vTaskResume(x_blink_task);
			}
			else
				ESP_LOGI(TAG, "Not delay or missing delay num");

			is_delay = false;
			delay_num_buffer = 500;
		}
		*data = '\0';
	}

}

static void configure_led(void) {

	gpio_reset_pin(BLINK_GPIO);
	/* Set the GPIO as a push/pull output */
	gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

}

static void blink_task(void *arg) {

	static uint8_t led_state = 0;
	static uint32_t led_count = 0;
	uint32_t delay_num = 500;
	char blink_buffer[BLINK_BUF_SIZE] = { 0 };
	while (1) {

		if (ulTaskNotifyTake(pdFALSE, 0)) {
			while (xQueueReceive(queue1, (void *) &delay_num, 0) == pdTRUE)
				ESP_LOGI(TAG, "in %s, delay number is %lu", __func__, delay_num);
		}
		gpio_set_level(BLINK_GPIO, led_state);
		if (led_state) {
			led_count++;
			if (led_count % BLINK_COUNT_INTERVAL == 0) {
				ESP_LOGD(TAG, "in %s: BLINKED %lu", __func__, led_count);
				snprintf(blink_buffer, BLINK_BUF_SIZE, "Blinked %lu", led_count);
				xQueueSend(queue2, (void *) blink_buffer, 10);
				xTaskNotifyGive(x_delay_task);
			}
		}
		led_state = !led_state;
		vTaskDelay(delay_num / portTICK_PERIOD_MS);
	}

}

void app_main(void)
{

	/* Configure parameters of an UART driver,
	 * communication pins and install the driver */
	uart_config_t uart_config = {
		.baud_rate = ECHO_UART_BAUD_RATE,
		.data_bits = UART_DATA_8_BITS,
		.parity    = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
	intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

	ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE, 0, 0, NULL, intr_alloc_flags));
	ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));


	configure_led();


	// Create queues
	queue1 = xQueueCreate(queue_len, sizeof(uint32_t));
	queue2 = xQueueCreate(queue_len, BLINK_BUF_SIZE);

	xTaskCreate(delay_task, "delay_task", DELAY_TASK_STACK_SIZE, NULL, 1, &x_delay_task);
	xTaskCreate(blink_task, "blink_task", BLINK_TASK_STACK_SIZE, NULL, 1, &x_blink_task);
}
