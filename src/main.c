/*
 * Copyright (c) 2019 Exploratory Engineering
 */
#include <zephyr.h>
#include <errno.h>
#include <stdio.h>
#include <nrf_socket.h>
#include <net/socket.h>
#include <dk_buttons_and_leds.h>

bool send_message(struct sockaddr_in dest, void *message, int size) {
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		printf("Error opening socket: %d\n", errno);
		return false;
	}
	
	if (sendto(sock, message, size, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		printf("Error sending: %d\n", errno);
		goto error;
	}

	printf("Message sent (size=%d)\n", size);

	close(sock);
	return true;

error:
	close(sock);
	return false;
}

bool get_gps_fix(int sock, nrf_gnss_data_frame_t *fix) {
	bool got_fix = false;

	while (true) {
		nrf_gnss_data_frame_t gps_data;
		int flags = got_fix ? NRF_MSG_DONTWAIT : 0;
		int retval = nrf_recv(sock, &gps_data, sizeof(nrf_gnss_data_frame_t), flags);
		if (retval <= 0) {
			break;
		}

		switch (gps_data.data_id) {
		case NRF_GNSS_PVT_DATA_ID:
			if ((gps_data.pvt.flags & NRF_GNSS_PVT_FLAG_FIX_VALID_BIT) == NRF_GNSS_PVT_FLAG_FIX_VALID_BIT) {
				memcpy(fix, &gps_data, sizeof(gps_data));
				got_fix = true;
			}
			break;
		case NRF_GNSS_NMEA_DATA_ID:
			break;
		}
	}

	return got_fix;
}

int exec_at_cmd(int sock, const char *cmd, char *response, int response_len) {
	k_sleep(100);

	if (send(sock, cmd, strlen(cmd), 0) < 0) {
		printf("Error sending: %d\n", errno);
		return -1;
	}

	char buf[1024];
	int n = recv(sock, buf, sizeof(buf), 0);
	if (n < 0) {
		printf("Error receiving: %d\n", errno);
		return -1;
	}

	int ret_response_len = 0;

	char *p = buf;
	char *end = buf + n;
	while (p < end) {
		if (memcmp(p, "ERROR", 5) == 0) {
			printf("Command '%s' responded with ERROR.\n", cmd);
			return -1;
		}
		if (memcmp(p, "OK", 2) == 0) {
			break;
		}

		char *next = memchr(p, '\r', end-p);
		if (next == NULL) {
			next = end;
		}
		if (response != NULL) {
			ret_response_len = MIN(response_len, next-p);
			memcpy(response, p, ret_response_len);
		}

		// skip "\r\n"
		p = next + 2;
	}

	return ret_response_len;
}

bool set_apn() {
	int sock = socket(AF_LTE, 0, NPROTO_AT);
	if (sock < 0) {
		printf("Error opening socket: %d\n", errno);
		return false;
	}

	if (exec_at_cmd(sock, "AT+CFUN=1", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT+CGATT=0", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT+CGDCONT=0,\"IP\",\"mda.ee\"", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT+CGDCONT?", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT+CGATT=1", NULL, 0) < 0) {
		goto error;
	}
	while (true) {
		char resp[32];
		int n = exec_at_cmd(sock, "AT+CEREG?", resp, sizeof(resp));
		if (n < 0) {
			goto error;
		}
		if (n >= 10 && memcmp(resp, "+CEREG", 6) == 0 && resp[10] == '1') {
			break;
		}
	}

	close(sock);
	return true;

error:
	close(sock);
	return false;
}

int gps_socket() {
	int sock = nrf_socket(NRF_AF_LOCAL, NRF_SOCK_DGRAM, NRF_PROTO_GNSS);
	if (sock < 0) {
		printk("Could not init GPS socket (err: %d)\n", sock);
		return -1;
	}

	u16_t fix_retry = 0;
	int retval = nrf_setsockopt(sock,
				NRF_SOL_GNSS,
				NRF_SO_GNSS_FIX_RETRY,
				&fix_retry,
				sizeof(uint16_t));

	if (retval != 0) {
		printk("Failed to set fix retry value\n");
		return -1;
	}

	u16_t fix_interval = 1;
	retval = nrf_setsockopt(sock,
				NRF_SOL_GNSS,
				NRF_SO_GNSS_FIX_INTERVAL,
				&fix_interval,
				sizeof(uint16_t));

	if (retval != 0) {
		printk("Failed to set fix interval value\n");
		return -1;
	}

	u8_t use_case = 0;
	retval = nrf_setsockopt(sock,
				NRF_SOL_GNSS,
				NRF_SO_GNSS_USE_CASE,
				&use_case,
				sizeof(uint8_t));

	if (retval != 0) {
		printk("Failed to set use case value\n");
		return -1;
	}

	retval = nrf_setsockopt(sock,
				NRF_SOL_GNSS,
				NRF_SO_GNSS_START,
				NULL,
				0);

	if (retval != 0) {
		printk("Failed to start GPS\n");
		return -1;
	}

	printk("GPS socket created\n");

	return sock;
}

bool close_gps_socket(int sock) {
	int retval = nrf_setsockopt(sock,
				NRF_SOL_GNSS,
				NRF_SO_GNSS_STOP,
				NULL,
				0);

	if (retval != 0) {
		printk("Failed to stop GPS\n");
		return false;
	}
	retval = close(sock);
	if (retval != 0) {
		printk("Failed to close GPS socket\n");
		return false;
	}
	printk("GPS socket closed\n");
	return true;
}

bool systemmode_lte_gps() {
	int sock = socket(AF_LTE, 0, NPROTO_AT);
	if (sock < 0) {
		printf("Error opening socket: %d\n", errno);
		return false;
	}

	if (exec_at_cmd(sock, "AT+CFUN=4", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT%XSYSTEMMODE=1,0,1,0", NULL, 0) < 0 ||
		exec_at_cmd(sock, "AT+CPSMS=1,\"\",\"\",\"11111111\",\"00000000\"", NULL, 0) < 0 || // disable TAU, go to PSM immediately
		exec_at_cmd(sock, "AT+CFUN=1", NULL, 0) < 0) {
		printf("Error enabling system mode LTE+GPS: %d\n", errno);
		goto error;
	}
	printf("System mode LTE+GPS.\n");

	while (true) {
		char resp[32];
		int n = exec_at_cmd(sock, "AT+CEREG?", resp, sizeof(resp));
		if (n < 0) {
			goto error;
		}
		if (n >= 10 && memcmp(resp, "+CEREG", 6) == 0 && resp[10] == '1') {
			break;
		}
	}

	close(sock);
	return true;

error:
	close(sock);
	return false;
}

bool print_imei_imsi() {
	int sock = socket(AF_LTE, 0, NPROTO_AT);
	if (sock < 0) {
		printf("Error opening socket: %d\n", errno);
		return false;
	}

	char resp[16];
	int n = exec_at_cmd(sock, "AT+CGSN", resp, sizeof(resp));
	if (n < 0) {
		printf("Error getting IMEI.\n");
		goto error;
	}
	resp[n] = '\0';
	printf("IMEI: %s\n", resp);

	n = exec_at_cmd(sock, "AT+CIMI", resp, sizeof(resp));
	if (n < 0) {
		printf("Error getting IMSI.\n");
		goto error;
	}
	resp[n] = '\0';
	printf("IMSI: %s\n", resp);

	close(sock);
	return true;

error:
	close(sock);
	return false;
}

// Partly copy/paste from zephyr/subsys/net/ip/utils.c
// TODO: Remove when Zephyr is fixed.
int tmp_net_addr_pton(sa_family_t family, const char *src, void *dst) {
	if (family == AF_INET) {
		struct in_addr *addr = (struct in_addr *)dst;
		size_t i, len;

		len = strlen(src);
		for (i = 0; i < len; i++) {
			if (!(src[i] >= '0' && src[i] <= '9') &&
				src[i] != '.') {
				return -EINVAL;
			}
		}

		(void)memset(addr, 0, sizeof(struct in_addr));

		for (i = 0; i < sizeof(struct in_addr); i++) {
			char *endptr;

			addr->s4_addr[i] = strtol(src, &endptr, 10);

			src = ++endptr;
		}
	} else {
		return -ENOTSUP;
	}

	return 0;
}

struct sockaddr_in make_addr() {
	const char* ip = "172.16.15.14";
	int port = 1234;

	struct sockaddr_in dest = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	tmp_net_addr_pton(AF_INET, ip, &dest.sin_addr);

	return dest;
}

typedef struct {
	float latitude;
	float longitude;
	float altitude;
} position;

void main() {
	printf("GPS application started.\n"); 

	if (dk_leds_init() != 0) {
		printf("Error initializing LEDs.\n");
	}

	if (dk_set_leds_state(0, DK_ALL_LEDS_MSK) != 0) {
		printf("Error setting LED state.\n");
	}

	struct sockaddr_in dest = make_addr();

	if (!systemmode_lte_gps()) {
		goto end;
	}
	if (!print_imei_imsi()) {
		printf("Failed to get IMEI/IMSI.\n");
		goto end;
	}

	if (!set_apn()) {
		printf("Failed to set APN.\n");
		goto end;
	}
	printf("Connected!\n"); 

	int sock = gps_socket();
	if (sock <= 0) {
		goto end;
	}

	while (true) {
		dk_set_led_on(DK_LED1);

		const int numPositions = 33;
		position positions[numPositions];
		for (int i = 0; i < numPositions; ++i) {
			nrf_gnss_data_frame_t gps_fix;
			while (!get_gps_fix(sock, &gps_fix)) {}
			positions[i].latitude = gps_fix.pvt.latitude;
			positions[i].longitude = gps_fix.pvt.longitude;
			positions[i].altitude = gps_fix.pvt.altitude;

			printf("Got GPS data: %02u-%02u-%02uT%02u:%02u:%02u, %f,%f,%f\n",
				gps_fix.pvt.datetime.year, gps_fix.pvt.datetime.month, gps_fix.pvt.datetime.day,
				gps_fix.pvt.datetime.hour,gps_fix.pvt.datetime.minute, gps_fix.pvt.datetime.seconds,
				gps_fix.pvt.latitude, gps_fix.pvt.longitude, gps_fix.pvt.altitude);

			static u32_t led_state = 1;
			dk_set_led(DK_LED3, led_state);
			led_state = 1 - led_state;
		}
		printf("Finished receiving GPS data\n");

		dk_set_led_off(DK_LED3);
		dk_set_led_off(DK_LED1);

		dk_set_led_on(DK_LED2);
		dk_set_led_off(DK_LED4);
		while (!send_message(dest, positions, sizeof(positions))) {
			printf("Failed to send.\n");
			k_sleep(1000);
			continue;
		}
		dk_set_led_on(DK_LED4);
		dk_set_led_off(DK_LED2);
	}

end:
	printf("GPS application complete.\n"); 
}
