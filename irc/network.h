#ifndef NETWORK_H
#define NETWORK_H

struct account;
struct channel;
struct channel_person;
struct person;

struct network
{
	struct irc_connection* irc_connection;
	struct account* accounts;
	struct channel* channels;
	struct person* people;
	struct channel_person* channel_people;
	struct scrollback* scrollbacks;
	char* nick;
	char* real_name;
	char* password;
	char* server_hostname;
	const char* autojoin;
};

#endif
