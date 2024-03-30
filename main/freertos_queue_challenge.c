#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG = "[ECHO] ";

// use only 1 core
// enable CONFIG_FREERTOS_UNICORE

#define ECHO_TEST_TXD GPIO_NUM_1
#define ECHO_TEST_RXD GPIO_NUM_3
// no flow control
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      UART_NUM_0
#define ECHO_UART_BAUD_RATE     115200
#define ECHO_TASK_STACK_SIZE    2*1024

#define BUF_SIZE 256
#define DELAY_BUF_SIZE 5

static bool readUntilWhitespaceOrDelay(size_t cmd_word_size, const char* cmd_word)
{

	bool is_cmd_word = true;
	int i = 1, c = 0;
	int len = uart_read_bytes(ECHO_UART_PORT_NUM, &c, 1, 20 / portTICK_PERIOD_MS);
	while (c != ' ' && c != '\t' && c != '\r' && i < cmd_word_size) {

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
	return is_cmd_word;
}

static void echo_task(void *arg)
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

	// Configure a temporary buffer for the incoming data
	uint8_t *data = (uint8_t *) calloc(2, sizeof(uint8_t));
	bool is_delay = false;
	const char* delay_cmd = "delay";

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
			if (readUntilWhitespaceOrDelay(DELAY_BUF_SIZE, delay_cmd))
				ESP_LOGI(TAG, "It is you delay!");
			else
				ESP_LOGI(TAG, "Not delay");

			is_delay = false;
		}
		*data = '\0';
	}

	free(data);

}

void app_main(void)
{
	xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 1, NULL);
}
