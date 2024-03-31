#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG = "[QUEUE] ";

// use only 1 core
// enable CONFIG_FREERTOS_UNICORE

#define ECHO_TEST_TXD GPIO_NUM_1
#define ECHO_TEST_RXD GPIO_NUM_3
// no flow control
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      UART_NUM_0
#define ECHO_UART_BAUD_RATE     115200

#define DELAY_TASK_STACK_SIZE    2*1024
#define BLINK_TASK_STACK_SIZE    2*1024

#define BUF_SIZE 256
#define DELAY_BUF_SIZE 6

// Task Handles
static TaskHandle_t x_delay_task = NULL, x_blink_task = NULL;

// Queue handles
static QueueHandle_t queue1;
static const uint8_t queue_len = 10;

static bool readDelayNum(uint32_t *delay_num_buffer) {

	uint8_t c = 0;
	bool is_delay_num = true;
	int len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	while (c != ' ' && c != '\t' && c != '\r' && *delay_num_buffer < UINT32_MAX) {
		if (len > 0 && c > 0) {
			uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) &c, len);
			if ((c > '9' && c < '0') || (*delay_num_buffer > *delay_num_buffer*10 + (c - '0'))) {
				is_delay_num = false;
				break;
			}

			*delay_num_buffer *= 10;
			*delay_num_buffer += (c - '0');
		}
		len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	}

	return is_delay_num;
}

static bool readUntilWhitespaceOrDelay(size_t cmd_word_size, const char* cmd_word, uint32_t* delay_num_buffer)
{

	bool is_cmd_word = true;
	uint8_t c = 0;
	int i = 1;
	int len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	while (c != '\t' && c != '\r' && i < cmd_word_size) {

		if (len > 0 && c > 0) {
			uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) &c, len);
			if (c == 'd') {
				i = 0;
				ESP_LOGI(TAG, "Delay soon?");
			} else if (c != cmd_word[i]) {
				is_cmd_word = false;
				break;
			}
			i++;
		}
		len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	}

	return is_cmd_word && readDelayNum(delay_num_buffer);
}

static void delay_task(void *arg)
{

	// Configure a temporary buffer for the incoming data
	uint8_t *data = (uint8_t *) calloc(2, sizeof(uint8_t));
	bool is_delay = false;
	uint32_t delay_num_buffer = 0;
	const char* delay_cmd = "delay ";

	while (1) {
		// Read data from the UART
		int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, 1, 20 / portTICK_PERIOD_MS);
		if (*data == '\r')
			*data = '\n';
		else if (*data == 'd') {
			ESP_LOGI(TAG, "Delay, is that you?");
			is_delay = true;
		}

		// Write data back to the UART
		uart_write_bytes(ECHO_UART_PORT_NUM, (const char*) data, len);
		if (len) {
			data[len] = '\0';
			ESP_LOGI(TAG, "Recv str: %s", (char *) data);
		}

		if (is_delay) {
			if (readUntilWhitespaceOrDelay(DELAY_BUF_SIZE, delay_cmd, &delay_num_buffer)) {
				ESP_LOGI(TAG, "delay %lu", delay_num_buffer);
				xQueueSend(queue1, (void *) &delay_num_buffer, 10);

				// Send notification to blink_task, bringing it out of the Blocked state
				xTaskNotifyGive(x_blink_task);
				// Block to wait for x_blink_task to notify this task
				ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

			}
			else
				ESP_LOGI(TAG, "Not delay or missing delay num");

			is_delay = false;
			delay_num_buffer = 0;
		}
		*data = '\0';
	}

	free(data);

}

static void blink_task(void *arg) {

	uint32_t delay_num = 0;
	while (1) {

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		xQueueReceive(queue1, (void *) &delay_num, 0);
		ESP_LOGI(TAG, "In %s, delay number is %lu", __func__, delay_num);
		xTaskNotifyGive(x_delay_task);

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

	// Create queues
	queue1 = xQueueCreate(queue_len, sizeof(uint32_t));

	xTaskCreate(delay_task, "delay_task", DELAY_TASK_STACK_SIZE, NULL, 1, &x_delay_task);
	// Start Task B
	xTaskCreate(blink_task, "blink_task", BLINK_TASK_STACK_SIZE, NULL, 1, &x_blink_task);
}
