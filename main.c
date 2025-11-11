#include <fcntl.h>
#include <gpiod.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#define BLK_TXT_WH_BKG "\033[30;47m"
#define RST "\033[0m"
#define CLR_LINE "\033[K\r                          \r"
// yeah who gives a fuck, sue me

int g_stop = 0;
const char *CHIPNAME = "/dev/gpiochip0";
const unsigned int RELAY = 17; // GPIO17
const unsigned int MOTOR = 27; // GPIO27

void sig_stop(int sig) {
  printf("\n\nGoodbye!\n\n");
  g_stop = 1;
}

int setup_gpio(struct gpiod_chip **chip, struct gpiod_line_settings **settings,
               struct gpiod_line_config **line_cfg,
               struct gpiod_request_config **req_cfg,
               struct gpiod_line_request **request

) {
  enum gpiod_line_value values[2];
  unsigned int offsets[2] = {MOTOR, RELAY};
  *chip = gpiod_chip_open(CHIPNAME);
  if (!*chip) {
    perror("gpiod_chip_open");
    return -1;
  }

  *settings = gpiod_line_settings_new();
  if (!*settings) {
    perror("gpiod_line_settings_new");
    return -1;
  }

  gpiod_line_settings_set_direction(*settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(*settings, GPIOD_LINE_VALUE_INACTIVE);

  *line_cfg = gpiod_line_config_new();
  if (!*line_cfg) {
    perror("gpiod_line_config_new");
    return -1;
  }

  if (gpiod_line_config_add_line_settings(*line_cfg, offsets, 2, *settings) <
      0) {
    perror("gpiod_line_config_add_line_settings");
    return -1;
  }

  *req_cfg = gpiod_request_config_new();
  if (!*req_cfg) {
    perror("gpiod_request_config_new");
    return -1;
  }

  gpiod_request_config_set_consumer(*req_cfg, "dual-toggle");

  *request = gpiod_chip_request_lines(*chip, *req_cfg, *line_cfg);
  if (!*request) {
    perror("gpiod_chip_request_lines");
    return -1;
  }

  return 0;
}

void closeout(struct gpiod_chip **chip, struct gpiod_line_settings **settings,
              struct gpiod_line_config **line_cfg,
              struct gpiod_request_config **req_cfg,
              struct gpiod_line_request **request) {

  if (*request)
    gpiod_line_request_release(*request);
  if (*req_cfg)
    gpiod_request_config_free(*req_cfg);
  if (*line_cfg)
    gpiod_line_config_free(*line_cfg);
  if (*settings)
    gpiod_line_settings_free(*settings);
  if (*chip)
    gpiod_chip_close(*chip);
}

enum STATUS { IDLE = 0, FORWARD, REVERSE };

int main() {
  struct gpiod_chip *chip = NULL;
  struct gpiod_line_settings *settings = NULL;
  struct gpiod_line_config *line_cfg = NULL;
  struct gpiod_request_config *req_cfg = NULL;
  struct gpiod_line_request *request = NULL;

  // connect to the gpio pins
  if (setup_gpio(&chip, &settings, &line_cfg, &req_cfg, &request) < 0) {
    closeout(&chip, &settings, &line_cfg, &req_cfg, &request);
    return 1;
  }

  int pipe_fds[2] = {0};
  // set up the messaging between userspace and daemon
  if (pipe(pipe_fds) < 0) {
    closeout(&chip, &settings, &line_cfg, &req_cfg, &request);
    return 1;
  }

  pid_t pid = fork();
  if (pid == 0) {
    if (setsid() < 0)
      return -1;

    pid = fork();
    if (pid > 0)
      return 0;
    if (pid < 0)
      return -1;

    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    close(pipe_fds[1]);
    // closing write end

    syslog(LOG_NOTICE, "Fishfeeder GPIO Daemon started");

    char buff = 0;
    ssize_t ret = read(pipe_fds[0], &buff, sizeof(char));
    if (ret < 0) {
      syslog(LOG_ERR, "Fishfeeder GPIO Daemon read() fail: %m");
    } else {
      syslog(LOG_NOTICE, "Fishfeeder GPIO Daemon: shutting down GPIO");
    }
    gpiod_line_request_set_value(request, MOTOR, GPIOD_LINE_VALUE_INACTIVE);
    gpiod_line_request_set_value(request, RELAY, GPIOD_LINE_VALUE_INACTIVE);
    closeout(&chip, &settings, &line_cfg, &req_cfg, &request);

    return 0;

  } // daemon

  close(pipe_fds[0]);
  // close reading end

  signal(SIGINT, sig_stop);
  struct termios defaultTerm, newTerm;

  // Get the current terminal settings
  tcgetattr(STDIN_FILENO, &defaultTerm);
  newTerm = defaultTerm;

  // Disable echo (ECHO) and canonical mode (ICANON)
  newTerm.c_lflag &= ~(ICANON | ECHO);

  // Set non-blocking input (using O_NONBLOCK)
  newTerm.c_cc[VMIN] = 1;  // Minimum number of characters to read
  newTerm.c_cc[VTIME] = 0; // Timeout (no timeout)

  // Apply the new terminal settings
  tcsetattr(STDIN_FILENO, TCSANOW, &newTerm);
  int defaultFlags = fcntl(STDIN_FILENO, F_GETFL);
  if (defaultFlags < 0) {
    perror("fcntl(F_GETFL)");
    return 1;
  }

  if (fcntl(0, F_SETFL, defaultFlags | O_NONBLOCK) < 0) {
    perror("fcntl(F_GETFL)");
    return 1;
  }

  printf("\033[H\033[2J");
  printf("Fish Feeder Terminal\n\n");
  printf(BLK_TXT_WH_BKG "f" RST " Forward " BLK_TXT_WH_BKG "r" RST
                        " Reverse " BLK_TXT_WH_BKG "^C" RST " Stop\n\n");

  int input = 0;
  enum STATUS status = IDLE;
  enum STATUS prev = status;
  do {
    if (EOF == (input = getchar())) {
      continue;
    }
    if ('f' == input) {
      status = FORWARD;
    } else if ('r' == input) {
      status = REVERSE;
    } else {
      continue;
    }

    if (prev == status) {
      // stopping what we are doing
      printf(CLR_LINE "Stopping %s....",
             (status == FORWARD ? "forward" : "reverse"));
      fflush(stdout);
      status = IDLE;
      if (gpiod_line_request_set_value(request, MOTOR,
                                       GPIOD_LINE_VALUE_INACTIVE) < 0) {
        perror("gpiod_line_request_set_value");
      }
    } else {
      // switching to different direction
      if (prev == IDLE) {
        printf(status == FORWARD ? CLR_LINE "Forward...."
                                 : CLR_LINE "Reverse....");
        // if we want to start off in reverse, set the relay to go in reverse
        if (gpiod_line_request_set_value(request, RELAY,
                                         status == REVERSE
                                             ? GPIOD_LINE_VALUE_ACTIVE
                                             : GPIOD_LINE_VALUE_INACTIVE) < 0) {
          perror("gpiod_line_request_set_value");
        }
        // otherwise just run the motor
        if (gpiod_line_request_set_value(request, MOTOR,
                                         GPIOD_LINE_VALUE_ACTIVE) < 0) {
          perror("gpiod_line_request_set_value");
        }
      } else {
        printf(CLR_LINE "Switching to %s",
               (status == FORWARD ? "Forward" : "Reverse"));
        if (status == FORWARD) {
          // if we want to go forward, then set the relay to off
          if (gpiod_line_request_set_value(request, RELAY,
                                           GPIOD_LINE_VALUE_INACTIVE) < 0) {
            perror("gpiod_line_request_set_value");
          }
        } else {
          // otherwise set it on, which changes the direction of the motor
          if (gpiod_line_request_set_value(request, RELAY,
                                           GPIOD_LINE_VALUE_ACTIVE) < 0) {
            perror("gpiod_line_request_set_value");
          }
        }
      }
      fflush(stdout);
    }
    prev = status;

  } while (!g_stop);

  tcsetattr(STDIN_FILENO, TCSANOW, &defaultTerm);
  fcntl(0, F_SETFL, defaultFlags);
  gpiod_line_request_set_value(request, MOTOR, GPIOD_LINE_VALUE_INACTIVE);
  gpiod_line_request_set_value(request, RELAY, GPIOD_LINE_VALUE_INACTIVE);
  closeout(&chip, &settings, &line_cfg, &req_cfg, &request);
  close(pipe_fds[1]);

  return 0;
}
