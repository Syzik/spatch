#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "parse.h"

#define PRINT_STATUS_DELAY 15

struct serv server1;
struct serv server2;
struct serv server3;
struct loginserv *logserv;

static int is_pass_auth_msg(ssh_message message) {
  return ssh_message_type(message) == SSH_REQUEST_AUTH
    && ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD;
}

static int is_chan_open_msg(ssh_message message) {
  return ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN
    && ssh_message_subtype(message) == SSH_CHANNEL_SESSION;
}

static int is_shell_request_msg(ssh_message message) {
  return ssh_message_type(message) == SSH_REQUEST_CHANNEL
    && (ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL
     || ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_PTY);
}

static int is_channel_closed_or_eof(ssh_channel chan) {
  return ssh_channel_is_eof(chan) || ssh_channel_is_closed(chan);
}

static int terminal_resize_callback(ssh_session session, ssh_channel channel,
				    int width, int height,
				    int pxwidth, int pxheight,
				    void *userdata) {
  printf("request resize %i %i %i %i\n", width, height, pxwidth, pxheight);
  return -1;
  // return 0; // accept resize
}

static int pty_request_callback(ssh_session session, ssh_channel channel,
				const char *term, int width, int height,
				int pxwidth, int pwheight, void *userdata) {
  printf("pty_request_callback");
}

static void channel_signal_callback(ssh_session session, ssh_channel channel,
				    char const *sig, void *userdata) {
  printf("signal : %s\n", sig);
}

static int channel_data(ssh_session session, ssh_channel channel, void *data,
			 uint32_t len, int is_stderr, void *userdata) {
  printf("channel_data\n");
}

static int packet_rcv_callback(ssh_session session, uint8_t type, ssh_buffer packet, void *user) {
  printf("packet rcv");
}

static void connect_channels(ssh_channel chan1, ssh_channel chan2, int timeout_ms) {
  char buffer[2048];
  int count;

  count = ssh_channel_read_timeout(chan1, buffer, sizeof(buffer), 0, timeout_ms);
  ssh_channel_write(chan2, buffer, count);
  count = ssh_channel_read_timeout(chan1, buffer, sizeof(buffer), 1, timeout_ms);
  ssh_channel_write_stderr(chan2, buffer, count);

  count = ssh_channel_read_timeout(chan2, buffer, sizeof(buffer), 0, timeout_ms);
  ssh_channel_write(chan1, buffer, count);
  count = ssh_channel_read_timeout(chan2, buffer, sizeof(buffer), 1, timeout_ms);
  ssh_channel_write_stderr(chan1, buffer, count);
}

static void channel_get_line(ssh_channel chan, char *buffer, int bufflen) {
  bzero(buffer, bufflen);
  int pos = 0;
  char c;

  while (1) {
    if (ssh_channel_read(chan, &c, 1, 0) != 1)
      break;
    if (c > 31)
      buffer[pos] = c;
    ssh_channel_write(chan, &c, 1);
    if (c == 127) {
      ssh_channel_write(chan, "\r", 1);
      for (int i = 0; i < pos; i++)
	ssh_channel_write(chan, " ", 1);
      if (pos > 0) {
	buffer[pos - 1] = 0;
	pos--;
      }
      ssh_channel_write(chan, "\r", 1);
      ssh_channel_write(chan, buffer, strlen(buffer));
    }
    else if (c == '\r') {
      ssh_channel_write(chan, "\r\n", 2);
      return;
    }
    else if (pos < bufflen-2)
      pos++;
  }
}

int verify_knownhost(ssh_session session, ssh_channel chan)
{
  int state, hlen;
  unsigned char *hash = NULL;
  char *hexa;
  char buf[10];
  char chanbuf[1024];
  
  state = ssh_is_server_known(session);
  hlen = ssh_get_pubkey_hash(session, &hash);
  
  if (hlen < 0)
    return 0;
  
  switch (state)
    {
    case SSH_SERVER_KNOWN_OK:
      break; /* ok */
    case SSH_SERVER_KNOWN_CHANGED:
      hexa = ssh_get_hexa(hash, hlen);
      snprintf(chanbuf, sizeof(chanbuf),
	       "Host key for server changed: it is now: %s\r\n"
	       "For security reasons, connection will be stopped\r\n",
	       hexa);
      ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
      free(hash);
      free(hexa);
      return 0;
    case SSH_SERVER_FOUND_OTHER:
      snprintf(chanbuf, sizeof(chanbuf),
	       "The host key for this server was not found but an other type of key exists.\r\n"
	       "An attacker might change the default server key to "
	       "confuse your client into thinking the key does not exist\r\n");
      ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
      free(hash);
      return 0;
    case SSH_SERVER_FILE_NOT_FOUND:
      snprintf(chanbuf, sizeof(chanbuf),
	       "Could not find known host file.\r\n"
	       "If you accept the host key here, the file will be automatically created.\r\n");
      ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
      /* fallback to SSH_SERVER_NOT_KNOWN behavior */
      case SSH_SERVER_NOT_KNOWN:
      hexa = ssh_get_hexa(hash, hlen);
      snprintf(chanbuf, sizeof(chanbuf),
	       "The server is unknown. Do you trust the host key?\r\n"
	       "Public key hash: %s\r\n", hexa);
      ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
      free(hexa);
      channel_get_line(chan, buf, sizeof(buf));
      if (strncasecmp(buf, "yes", 3) != 0) {
	free(hash);
	return 0;
      }
      if (ssh_write_knownhost(session) < 0) {
	snprintf(chanbuf, sizeof(chanbuf),
		 "Error %s\r\n", strerror(errno));
	ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
	free(hash);
	return 0;
      }
      break;
    case SSH_SERVER_ERROR:
      snprintf(chanbuf, sizeof(chanbuf), "Error %s", ssh_get_error(session));
      ssh_channel_write_stderr(chan, chanbuf, strlen(chanbuf));
      free(hash);
      return 0;
    }
  free(hash);
  return 1;
}

static void connect_to_host(ssh_channel client_chan, const char *usr_spatch,
			    const char *user, const char *password,
			    const char *hostname, int port) {
  const char *error_msg   = NULL;
  ssh_session session     = ssh_new();
  ssh_channel server_chan = NULL;

  ssh_options_set(session, SSH_OPTIONS_HOST, "localhost");
  ssh_options_set(session, SSH_OPTIONS_PORT, &port);

  printf("%s is openning connection to %s@%s:%i\n", usr_spatch, user, hostname, port);
  if (ssh_connect(session) != SSH_OK)
    error_msg = "failed to connect to host\r\n";
  else if (ssh_userauth_password(session, user, password) != SSH_OK)
    error_msg = "authentication failed\r\n";
  else if (!verify_knownhost(session, client_chan))
    error_msg = "knownhost verification failed\r\n";
  else if ((server_chan = ssh_channel_new(session)) == NULL)
    error_msg = "failed to create channel\r\n";
  else if (ssh_channel_open_session(server_chan) != SSH_OK)
    error_msg = "failed to open remote shell session\r\n";
  else if (ssh_channel_request_pty(server_chan) != SSH_OK)
    error_msg = "pty request failed\r\n";
  else if (ssh_channel_request_shell(server_chan) != SSH_OK)
    error_msg = "failed to open remote shell\r\n";

  // ssh_set_log_level(SSH_LOG_FUNCTIONS); // DEBUG LOG
  struct ssh_channel_callbacks_struct cb = {
    .userdata = NULL,
    // .channel_data_function = channel_data,
    .channel_signal_function = channel_signal_callback,
    .channel_pty_request_function = pty_request_callback,
    .channel_pty_window_change_function = terminal_resize_callback
  };
  ssh_callbacks_init(&cb);
  ssh_set_channel_callbacks(client_chan, &cb);

  ssh_event callback_poll = ssh_event_new();
  ssh_event_add_session(callback_poll, ssh_channel_get_session(client_chan));
  
  time_t print_status_time = time(NULL);

  if (error_msg)
    ssh_channel_write(client_chan, error_msg, strlen(error_msg));
  else {
    ssh_channel_change_pty_size(server_chan, 116, 64);  

    while (!is_channel_closed_or_eof(client_chan)
	   && !is_channel_closed_or_eof(server_chan)) {

      connect_channels(client_chan, server_chan, 10);
      if (time(NULL) >= print_status_time) {
	printf("%s is connected to shell on %s\n", usr_spatch, hostname);
	print_status_time = time(NULL) + PRINT_STATUS_DELAY;
      }

      ssh_event_dopoll(callback_poll, 10);
    }
  }

  printf("%s is disconnected from %s\n", user, hostname);
  ssh_channel_free(server_chan);
  ssh_free(session);
}

static int check_allowed_server(struct loginserv *login,
				 struct serv *server) {
  const char *user = login->spatch.user;
  const char *pass = login->spatch.password;
  struct allowed_user *list = server->listuser;

  while (list != NULL) {
    if (strcmp(list->user.user, user) == 0
	&& strcmp(list->user.password, pass) == 0) {
      return 1;
    }
    list = list->next;
  }
  return 0;
}

static int show_allowed_servers(struct loginserv *login,
				ssh_channel chan) {
  int c = 0;
  
  if (check_allowed_server(login, &server1)) {
    ssh_channel_write(chan, server1.adresse, strlen(server1.adresse));
    ssh_channel_write(chan, "\r\n", 2);
    c++;
  }
  if (check_allowed_server(login, &server2)) {
    ssh_channel_write(chan, server2.adresse, strlen(server2.adresse));
    ssh_channel_write(chan, "\r\n", 2);
    c++;
  }
  if (check_allowed_server(login, &server3)) {
    ssh_channel_write(chan, server3.adresse, strlen(server3.adresse));
    ssh_channel_write(chan, "\r\n", 2);
    c++;
  }
  return c;
}

static struct serv *match_server(const char *hostname,
				 struct loginserv *login,
				 char **user, char **password) {
  if (strcmp(server1.adresse, hostname) == 0
      && check_allowed_server(login, &server1)) {
    *user = login->serv1.user;
    *password = login->serv1.password;
    return &server1;
  }
  if (strcmp(server2.adresse, hostname) == 0
      && check_allowed_server(login, &server2)) {
    *user = login->serv2.user;
    *password = login->serv2.password;
    return &server2;
  }
  if (strcmp(server3.adresse, hostname) == 0
      && check_allowed_server(login, &server3)) {
    *user = login->serv3.user;
    *password = login->serv3.password;
    return &server3;
  }
  return NULL;
}

static void select_host(ssh_channel chan, const char *user,
			struct loginserv *login) {
  const char *welcome_msg = "welcome to spatch\r\n";
  const char *select_msg  = "select an endpoint\r\n";
  const char *nendp_msg   = "no valid endpoint\r\n";
  char        buffer[1024];
  time_t      print_status_time = time(NULL);
  struct serv *server = NULL;

  ssh_channel_write(chan, welcome_msg, strlen(welcome_msg));
  do {
    if (time(NULL) >= print_status_time) {
      printf("%s is connected to spatch\n", user);
      print_status_time = time(NULL) + PRINT_STATUS_DELAY;
    }

    ssh_channel_write(chan, select_msg, strlen(select_msg));
    if (show_allowed_servers(login, chan) == 0) {
      ssh_channel_write(chan, nendp_msg, strlen(nendp_msg));
      break;
    }
    ssh_channel_write(chan, "exit\r\n", strlen("exit\r\n"));
    
    channel_get_line(chan, buffer, sizeof(buffer));

    char *svr_usr;
    char *svr_pass;
    if ((server = match_server(buffer, login, &svr_usr, &svr_pass)) != NULL) {
      connect_to_host(chan, user, svr_usr, svr_pass, buffer, server->port);
	break;
      }
  } while (strcmp(buffer, "exit") && !is_channel_closed_or_eof(chan));

  printf("%s disconnected\n", user);
}

struct loginserv *match_login(const char *user, const char *pass) {
  struct loginserv *login = logserv;
  
  while (login != NULL) {
    // printf("login %s %s\n", user, pass);
    // printf("match %s %s\n", login->spatch.user, login->spatch.password);
    if (strcmp(user, login->spatch.user) == 0
	&& strcmp(pass, login->spatch.password) == 0)
	break;
    login = login->next;
  }
  // printf("login %p\n", login);
  return login;
}

static void handle_session(ssh_session session) {
  if (ssh_handle_key_exchange(session) != SSH_OK) {
    fprintf(stderr, "key echange failed\n");
    return;
  }

  ssh_channel chan    = NULL;
  ssh_message message = NULL;
  int         auth    = 0;
  int         shell   = 0;
  char        user[64];
  int         limit   = 3;
  struct loginserv * login;

  printf("session\n");
  // authenticate user and open channel
  do {
    message = ssh_message_get(session);
    //if (message == NULL)
    //  break;

    if (is_pass_auth_msg(message)) {
      printf("auth\n");
      const char *user_tmp = strdup(ssh_message_auth_user(message));
      const char *pass     = ssh_message_auth_password(message);
      strncpy(user, user_tmp, sizeof(user));
      login = match_login(user, pass);
      // printf("user %s %s\n", login->spatch.user, login->spatch.password);
      // printf("user %s %s\n", user, pass);
      if (!login && --limit <= 0)
	break;
      auth = 1;
      ssh_message_auth_reply_success(message, 0);
    }
    else if (is_chan_open_msg(message)) {
      chan = ssh_message_channel_request_open_reply_accept(message);
    }
    else if (is_shell_request_msg(message)) {
      shell = 1;
      ssh_message_channel_request_reply_success(message);
    }
    else {
      ssh_message_reply_default(message);
    }

    ssh_message_free(message);
  } while (!auth || !chan || !shell);

  if (!auth) {
    fprintf(stderr, "authentication failed\n");
    return;
  }
  if (!chan) {
    fprintf(stderr, "failed to open channel\n");
    return;
  }
  if (!shell) {
    fprintf(stderr, "channel type not supported\n");
    return;
  }

  select_host(chan, user, login);

  ssh_channel_close(chan);
  ssh_channel_free(chan);
}

int main() {
  logserv = parse_config();
  if (parseFile(1, &server1) != 0
      || parseFile(2, &server2) != 0
      || parseFile(3, &server3) != 0) {
    fprintf(stderr, "failed to load server config files\n");
    return 1;
  }

  ssh_bind    bind;
  ssh_session session;
  
  bind = ssh_bind_new();

  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_RSAKEY, "/etc/ssh/ssh_host_rsa_key");
  // set to non blocking to reload the configuration
  // ssh_bind_set_blocking(bind, 0);
  
  if (ssh_bind_listen(bind) < 0) {
    fprintf(stderr, "%s\n", ssh_get_error(bind));
    return 1;
  }

  // loop to accept new connections
  while (1) {
    session = ssh_new();
    // ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);
    
    if (session == NULL) {
      fprintf(stderr, "failed to create new ssh session\n");
      return 1;
    }

    if (ssh_bind_accept(bind, session) != SSH_ERROR) {
      int pid = fork();

      if (pid == 0) {
	handle_session(session);
	ssh_disconnect(session);
	ssh_free(session);
	break;
      }
      else if (pid < 1)
	fprintf(stderr, "fork error\n");
    }
    else {
      fprintf(stderr, "accept failed : %s\n", ssh_get_error(bind));
      return 1;
    }

    ssh_free(session);
  }
  
  ssh_bind_free(bind);
  return 0;
}
