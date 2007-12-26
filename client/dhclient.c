/* dhclient.c

   DHCP Client. */

/*
 * Copyright (c) 1996-1999 Internet Software Consortium.
 * Use is subject to license terms which appear in the file named
 * ISC-LICENSE that should have accompanied this file when you
 * received it.   If a file named ISC-LICENSE did not accompany this
 * file, or you are not sure the one you have is correct, you may
 * obtain an applicable copy of the license at:
 *
 *             http://www.isc.org/isc-license-1.0.html. 
 *
 * This file is part of the ISC DHCP distribution.   The documentation
 * associated with this file is listed in the file DOCUMENTATION,
 * included in the top-level directory of this release.
 *
 * Support and other services are available for ISC products - see
 * http://www.isc.org for more information.
 */

#ifndef lint
static char ocopyright[] =
"$Id: dhclient.c,v 1.62 1999/03/16 05:50:30 mellon Exp $ Copyright (c) 1995, 1996, 1997, 1998, 1999 The Internet Software Consortium.  All rights reserved.\n";
#endif /* not lint */

#include "dhcpd.h"

TIME cur_time;
TIME default_lease_time = 43200; /* 12 hours... */
TIME max_lease_time = 86400; /* 24 hours... */
struct tree_cache *global_options [256];

char *path_dhclient_conf = _PATH_DHCLIENT_CONF;
char *path_dhclient_db = _PATH_DHCLIENT_DB;
char *path_dhclient_pid = _PATH_DHCLIENT_PID;

int dhcp_max_agent_option_packet_length = 0;

int interfaces_requested = 0;

int log_perror = 1;

struct iaddr iaddr_broadcast = { 4, { 255, 255, 255, 255 } };
struct iaddr iaddr_any = { 4, { 0, 0, 0, 0 } };
struct in_addr inaddr_any;
struct sockaddr_in sockaddr_broadcast;

/* ASSERT_STATE() does nothing now; it used to be
   assert (state_is == state_shouldbe). */
#define ASSERT_STATE(state_is, state_shouldbe) {}

static char copyright[] =
"Copyright 1995, 1996, 1997, 1998, 1999 The Internet Software Consortium.";
static char arr [] = "All rights reserved.";
static char message [] = "Internet Software Consortium DHCP Client V3.0-alpha-990315";
static char contrib [] = "\nPlease contribute if you find this software useful.";
static char url [] = "For info, please visit http://www.isc.org/dhcp-contrib.html\n";

u_int16_t local_port;
u_int16_t remote_port;
int log_priority;
int no_daemon;
int save_scripts;

static void usage PROTO ((void));

int main (argc, argv, envp)
	int argc;
	char **argv, **envp;
{
	int i;
	struct servent *ent;
	struct interface_info *ip;
	struct client_state *client;
	int seed;
	int quiet = 0;

#ifdef SYSLOG_4_2
	openlog ("dhclient", LOG_NDELAY);
	log_priority = LOG_DAEMON;
#else
	openlog ("dhclient", LOG_NDELAY, LOG_DAEMON);
#endif

#if !(defined (DEBUG) || defined (SYSLOG_4_2) || defined (__CYGWIN32__))
	setlogmask (LOG_UPTO (LOG_INFO));
#endif	

	for (i = 1; i < argc; i++) {
		if (!strcmp (argv [i], "-p")) {
			if (++i == argc)
				usage ();
			local_port = htons (atoi (argv [i]));
			log_debug ("binding to user-specified port %d",
			       ntohs (local_port));
		} else if (!strcmp (argv [i], "-d")) {
			no_daemon = 1;
		} else if (!strcmp (argv [i], "-D")) {
			save_scripts = 1;
                } else if (!strcmp (argv [i], "-pf")) {
                        if (++i == argc)
                                usage ();
                        path_dhclient_pid = argv [i];
                } else if (!strcmp (argv [i], "-cf")) {
                        if (++i == argc)
                                usage ();
                        path_dhclient_conf = argv [i];
                } else if (!strcmp (argv [i], "-lf")) {
                        if (++i == argc)
                                usage ();
                        path_dhclient_db = argv [i];
		} else if (!strcmp (argv [i], "-q")) {
			quiet = 1;
			quiet_interface_discovery = 1;
 		} else if (argv [i][0] == '-') {
 		    usage ();
 		} else {
 		    struct interface_info *tmp =
 			((struct interface_info *)
 			 dmalloc (sizeof *tmp, "specified_interface"));
 		    if (!tmp)
 			log_fatal ("Insufficient memory to %s %s",
 			       "record interface", argv [i]);
 		    memset (tmp, 0, sizeof *tmp);
 		    strcpy (tmp -> name, argv [i]);
 		    tmp -> next = interfaces;
 		    tmp -> flags = INTERFACE_REQUESTED;
		    interfaces_requested = 1;
 		    interfaces = tmp;
 		}
	}

	if (!quiet) {
		log_info (message);
		log_info (copyright);
		log_info (arr);
		log_info (contrib);
		log_info (url);
	}

	/* Default to the DHCP/BOOTP port. */
	if (!local_port) {
		ent = getservbyname ("dhcpc", "udp");
		if (!ent)
			local_port = htons (68);
		else
			local_port = ent -> s_port;
#ifndef __CYGWIN32__
		endservent ();
#endif
	}
	remote_port = htons (ntohs (local_port) - 1);	/* XXX */
  
	/* Get the current time... */
	GET_TIME (&cur_time);

	sockaddr_broadcast.sin_family = AF_INET;
	sockaddr_broadcast.sin_port = remote_port;
	sockaddr_broadcast.sin_addr.s_addr = INADDR_BROADCAST;
#ifdef HAVE_SA_LEN
	sockaddr_broadcast.sin_len = sizeof sockaddr_broadcast;
#endif
	inaddr_any.s_addr = INADDR_ANY;

	/* Discover all the network interfaces. */
	discover_interfaces (DISCOVER_UNCONFIGURED);

	/* Parse the dhclient.conf file. */
	read_client_conf ();

	/* Parse the lease database. */
	read_client_leases ();

	/* Rewrite the lease database... */
	rewrite_client_leases ();

	/* XXX */
/* 	config_counter(&snd_counter, &rcv_counter); */

	/* If no broadcast interfaces were discovered, call the script
	   and tell it so. */
	if (!interfaces) {
		script_init ((struct client_state *)0, "NBI",
			     (struct string_list *)0);
		script_go ((struct client_state *)0);

		log_info ("No broadcast interfaces found - exiting.");

		/* Nothing more to do. */
		exit (0);
	} else {
		/* Call the script with the list of interfaces. */
		for (ip = interfaces; ip; ip = ip -> next) {
			/* If interfaces were specified, don't configure
			   interfaces that weren't specified! */
			if (interfaces_requested &&
			    ((ip -> flags & (INTERFACE_REQUESTED |
					     INTERFACE_AUTOMATIC)) !=
			     INTERFACE_REQUESTED))
				continue;
			script_init (ip -> client,
				     "PREINIT", (struct string_list *)0);
			if (ip -> client -> alias)
				script_write_params (ip -> client, "alias_",
						     ip -> client -> alias);
			script_go (ip -> client);
		}
	}

	/* At this point, all the interfaces that the script thinks
	   are relevant should be running, so now we once again call
	   discover_interfaces(), and this time ask it to actually set
	   up the interfaces. */
	discover_interfaces (interfaces_requested
			     ? DISCOVER_REQUESTED
			     : DISCOVER_RUNNING);

	/* Make up a seed for the random number generator from current
	   time plus the sum of the last four bytes of each
	   interface's hardware address interpreted as an integer.
	   Not much entropy, but we're booting, so we're not likely to
	   find anything better. */
	seed = 0;
	for (ip = interfaces; ip; ip = ip -> next) {
		int junk;
		memcpy (&junk,
			&ip -> hw_address.haddr [ip -> hw_address.hlen -
						 sizeof seed], sizeof seed);
		seed += junk;
	}
	srandom (seed + cur_time);

	/* Start a configuration state machine for each interface. */
	for (ip = interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			client -> state = S_INIT;
			state_reboot (client);
		}
	}

	/* Set up the bootp packet handler... */
	bootp_packet_handler = do_packet;

	/* Start dispatching packets and timeouts... */
	dispatch ();

	/*NOTREACHED*/
	return 0;
}

static void usage ()
{
	log_fatal ("Usage: dhclient [-d] [-D] [-q] [-c] [-p <port>]\n [-lf %s",
	       "lease-file] [-pf pid-file] [-cf config-file] [interface]");
}

void cleanup ()
{
}

struct class *find_class (s)
	char *s;
{
	return (struct class *)0;
}

int check_collection (packet, collection)
	struct packet *packet;
	struct collection *collection;
{
	return 0;
}

void classify (packet, class)
	struct packet *packet;
	struct class *class;
{
}

int unbill_class (lease, class)
	struct lease *lease;
	struct class *class;
{
	return 0;
}

/* Individual States:
 * 
 * Each routine is called from the dhclient_state_machine() in one of
 * these conditions:
 * -> entering INIT state
 * -> recvpacket_flag == 0: timeout in this state
 * -> otherwise: received a packet in this state
 *
 * Return conditions as handled by dhclient_state_machine():
 * Returns 1, sendpacket_flag = 1: send packet, reset timer.
 * Returns 1, sendpacket_flag = 0: just reset the timer (wait for a milestone).
 * Returns 0: finish the nap which was interrupted for no good reason.
 *
 * Several per-interface variables are used to keep track of the process:
 *   active_lease: the lease that is being used on the interface
 *                 (null pointer if not configured yet).
 *   offered_leases: leases corresponding to DHCPOFFER messages that have
 *		     been sent to us by DHCP servers.
 *   acked_leases: leases corresponding to DHCPACK messages that have been
 *		   sent to us by DHCP servers.
 *   sendpacket: DHCP packet we're trying to send.
 *   destination: IP address to send sendpacket to
 * In addition, there are several relevant per-lease variables.
 *   T1_expiry, T2_expiry, lease_expiry: lease milestones
 * In the active lease, these control the process of renewing the lease;
 * In leases on the acked_leases list, this simply determines when we
 * can no longer legitimately use the lease.
 */

void state_reboot (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	/* If we don't remember an active lease, go straight to INIT. */
	if (!client -> active ||
	    client -> active -> is_bootp) {
		state_init (client);
		return;
	}

	/* We are in the rebooting state. */
	client -> state = S_REBOOTING;

	/* make_request doesn't initialize xid because it normally comes
	   from the DHCPDISCOVER, but we haven't sent a DHCPDISCOVER,
	   so pick an xid now. */
	client -> xid = random ();

	/* Make a DHCPREQUEST packet, and set appropriate per-interface
	   flags. */
	make_request (client, client -> active);
	client -> destination = iaddr_broadcast;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Zap the medium list... */
	client -> medium = (struct string_list *)0;

	/* Send out the first DHCPREQUEST packet. */
	send_request (client);
}

/* Called when a lease has completely expired and we've been unable to
   renew it. */

void state_init (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	ASSERT_STATE(state, S_INIT);

	/* Make a DHCPDISCOVER packet, and set appropriate per-interface
	   flags. */
	make_discover (client, client -> active);
	client -> xid = client -> packet.xid;
	client -> destination = iaddr_broadcast;
	client -> state = S_SELECTING;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Add an immediate timeout to cause the first DHCPDISCOVER packet
	   to go out. */
	send_discover (client);
}

/* state_selecting is called when one or more DHCPOFFER packets have been
   received and a configurable period of time has passed. */

void state_selecting (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	struct client_lease *lp, *next, *picked;


	ASSERT_STATE(state, S_SELECTING);

	/* Cancel state_selecting and send_discover timeouts, since either
	   one could have got us here. */
	cancel_timeout (state_selecting, client);
	cancel_timeout (send_discover, client);

	/* We have received one or more DHCPOFFER packets.   Currently,
	   the only criterion by which we judge leases is whether or
	   not we get a response when we arp for them. */
	picked = (struct client_lease *)0;
	for (lp = client -> offered_leases; lp; lp = next) {
		next = lp -> next;

		/* Check to see if we got an ARPREPLY for the address
		   in this particular lease. */
		if (!picked) {
			picked = lp;
			picked -> next = (struct client_lease *)0;
		} else {
		      freeit:
			destroy_client_lease (lp);
		}
	}
	client -> offered_leases = (struct client_lease *)0;

	/* If we just tossed all the leases we were offered, go back
	   to square one. */
	if (!picked) {
		client -> state = S_INIT;
		state_init (client);
		return;
	}

	/* If it was a BOOTREPLY, we can just take the address right now. */
	if (picked -> is_bootp) {
		client -> new = picked;

		/* Make up some lease expiry times
		   XXX these should be configurable. */
		client -> new -> expiry = cur_time + 12000;
		client -> new -> renewal += cur_time + 8000;
		client -> new -> rebind += cur_time + 10000;

		client -> state = S_REQUESTING;

		/* Bind to the address we received. */
		bind_lease (client);
		return;
	}

	/* Go to the REQUESTING state. */
	client -> destination = iaddr_broadcast;
	client -> state = S_REQUESTING;
	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;

	/* Make a DHCPREQUEST packet from the lease we picked. */
	make_request (client, picked);
	client -> xid = client -> packet.xid;

	/* Toss the lease we picked - we'll get it back in a DHCPACK. */
	destroy_client_lease (picked);

	/* Add an immediate timeout to send the first DHCPREQUEST packet. */
	send_request (client);
}  

/* state_requesting is called when we receive a DHCPACK message after
   having sent out one or more DHCPREQUEST packets. */

void dhcpack (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;
	struct client_lease *lease;
	struct option_cache *oc;
	struct data_string ds;
	int i;
	
	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	for (client = ip -> client; client; client = client -> next) {
		if (client -> xid == packet -> raw -> xid)
			break;
	}
	if (!client ||
	    (packet -> interface -> hw_address.hlen !=
	     packet -> raw -> hlen) ||
	    (memcmp (packet -> interface -> hw_address.haddr,
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
		log_debug ("DHCPACK in wrong transaction.");
		return;
	}

	if (client -> state != S_REBOOTING &&
	    client -> state != S_REQUESTING &&
	    client -> state != S_RENEWING &&
	    client -> state != S_REBINDING) {
		log_debug ("DHCPACK in wrong state.");
		return;
	}

	log_info ("DHCPACK from %s", piaddr (packet -> client_addr));

	lease = packet_to_lease (packet);
	if (!lease) {
		log_info ("packet_to_lease failed.");
		return;
	}

	client -> new = lease;

	/* Stop resending DHCPREQUEST. */
	cancel_timeout (send_request, client);

	/* Figure out the lease time. */
	oc = lookup_option (client -> new -> options.dhcp_hash,
			    DHO_DHCP_LEASE_TIME);
	memset (&ds, 0, sizeof ds);
	if (oc &&
	    evaluate_option_cache (&ds, packet,
				   &client -> new -> options, oc)) {
		if (ds.len > 3)
			client -> new -> expiry = getULong (ds.data);
		else
			client -> new -> expiry = 0;
		data_string_forget (&ds, "dhcpack");
	} else
			client -> new -> expiry = 0;

	if (!client -> new -> expiry) {
		log_error ("no expiry time on offered lease.");
		/* XXX this is going to be bad - if this _does_
		   XXX happen, we should probably dynamically 
		   XXX disqualify the DHCP server that gave us the
		   XXX bad packet from future selections and
		   XXX then go back into the init state. */
		state_init (client);
		return;
	}

	/* A number that looks negative here is really just very large,
	   because the lease expiry offset is unsigned. */
	if (client -> new -> expiry < 0)
		client -> new -> expiry = TIME_MAX;
	/* Take the server-provided renewal time if there is one. */
	oc = lookup_option (client -> new -> options.dhcp_hash,
			    DHO_DHCP_RENEWAL_TIME);
	if (oc &&
	    evaluate_option_cache (&ds, packet,
				   &client -> new -> options, oc)) {
		if (ds.len > 3)
			client -> new -> renewal = getULong (ds.data);
		else
			client -> new -> renewal = 0;
		data_string_forget (&ds, "dhcpack");
	} else
			client -> new -> renewal = 0;

	/* If it wasn't specified by the server, calculate it. */
	if (!client -> new -> renewal)
		client -> new -> renewal =
			client -> new -> expiry / 2;

	/* Same deal with the rebind time. */
	oc = lookup_option (client -> new -> options.dhcp_hash,
			    DHO_DHCP_REBINDING_TIME);
	if (oc &&
	    evaluate_option_cache (&ds, packet,
				   &client -> new -> options, oc)) {
		if (ds.len > 3)
			client -> new -> rebind = getULong (ds.data);
		else
			client -> new -> rebind = 0;
		data_string_forget (&ds, "dhcpack");
	} else
			client -> new -> rebind = 0;

	if (!client -> new -> rebind)
		client -> new -> rebind =
			client -> new -> renewal +
				client -> new -> renewal / 2 +
					client -> new -> renewal / 4;

	client -> new -> expiry += cur_time;
	/* Lease lengths can never be negative. */
	if (client -> new -> expiry < cur_time)
		client -> new -> expiry = TIME_MAX;
	client -> new -> renewal += cur_time;
	if (client -> new -> renewal < cur_time)
		client -> new -> renewal = TIME_MAX;
	client -> new -> rebind += cur_time;
	if (client -> new -> rebind < cur_time)
		client -> new -> rebind = TIME_MAX;

	bind_lease (client);
}

void bind_lease (client)
	struct client_state *client;
{
	struct interface_info *ip = client -> interface;

	/* Remember the medium. */
	client -> new -> medium = client -> medium;

	/* Run the client script with the new parameters. */
	script_init (client, (client -> state == S_REQUESTING
			  ? "BOUND"
			  : (client -> state == S_RENEWING
			     ? "RENEW"
			     : (client -> state == S_REBOOTING
				? "REBOOT" : "REBIND"))),
		     client -> new -> medium);
	if (client -> active && client -> state != S_REBOOTING)
		script_write_params (client, "old_", client -> active);
	script_write_params (client, "new_", client -> new);
	if (client -> alias)
		script_write_params (client, "alias_", client -> alias);

	/* If the BOUND/RENEW code detects another machine using the
	   offered address, it exits nonzero.  We need to send a
	   DHCPDECLINE and toss the lease. */
	if (script_go (client)) {
		make_decline (client, client -> new);
		send_decline (client);
		destroy_client_lease (client -> new);
		client -> new = (struct client_lease *)0;
		state_init (client);
		return;
	}

	/* Write out the new lease. */
	write_client_lease (client, client -> new, 0);

	/* Replace the old active lease with the new one. */
	if (client -> active)
		destroy_client_lease (client -> active);
	client -> active = client -> new;
	client -> new = (struct client_lease *)0;

	/* Set up a timeout to start the renewal process. */
	add_timeout (client -> active -> renewal,
		     state_bound, client);

	log_info ("bound to %s -- renewal in %d seconds.",
	      piaddr (client -> active -> address),
	      client -> active -> renewal - cur_time);
	client -> state = S_BOUND;
	reinitialize_interfaces ();
	go_daemon ();
}  

/* state_bound is called when we've successfully bound to a particular
   lease, but the renewal time on that lease has expired.   We are
   expected to unicast a DHCPREQUEST to the server that gave us our
   original lease. */

void state_bound (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	int i;
	struct option_cache *oc;
	struct data_string ds;

	ASSERT_STATE(state, S_BOUND);

	/* T1 has expired. */
	make_request (client, client -> active);
	client -> xid = client -> packet.xid;

	memset (&ds, 0, sizeof ds);
	oc = lookup_option (client -> active -> options.dhcp_hash,
			    DHO_DHCP_SERVER_IDENTIFIER);
	if (oc &&
	    evaluate_option_cache (&ds, (struct packet *)0,
				   &client -> active -> options, oc)) {
		if (ds.len > 3) {
			memcpy (client -> destination.iabuf, ds.data, 4);
			client -> destination.len = 4;
		} else
			client -> destination = iaddr_broadcast;
	} else
		client -> destination = iaddr_broadcast;

	client -> first_sending = cur_time;
	client -> interval = client -> config -> initial_interval;
	client -> state = S_RENEWING;

	/* Send the first packet immediately. */
	send_request (client);
}  

int commit_leases ()
{
	return 0;
}

int write_lease (lease)
	struct lease *lease;
{
	return 0;
}

void db_startup ()
{
}

void bootp (packet)
	struct packet *packet;
{
	struct iaddrlist *ap;

	if (packet -> raw -> op != BOOTREPLY)
		return;

	/* If there's a reject list, make sure this packet's sender isn't
	   on it. */
	for (ap = packet -> interface -> client -> config -> reject_list;
	     ap; ap = ap -> next) {
		if (addr_eq (packet -> client_addr, ap -> addr)) {
			log_info ("BOOTREPLY from %s rejected.",
			      piaddr (ap -> addr));
			return;
		}
	}
	
	dhcpoffer (packet);

}

void dhcp (packet)
	struct packet *packet;
{
	struct iaddrlist *ap;
	void (*handler) PROTO ((struct packet *));
	char *type;

	switch (packet -> packet_type) {
	      case DHCPOFFER:
		handler = dhcpoffer;
		type = "DHCPOFFER";
		break;

	      case DHCPNAK:
		handler = dhcpnak;
		type = "DHCPNACK";
		break;

	      case DHCPACK:
		handler = dhcpack;
		type = "DHCPACK";
		break;

	      default:
		return;
	}

	/* If there's a reject list, make sure this packet's sender isn't
	   on it. */
	for (ap = packet -> interface -> client -> config -> reject_list;
	     ap; ap = ap -> next) {
		if (addr_eq (packet -> client_addr, ap -> addr)) {
			log_info ("%s from %s rejected.",
			      type, piaddr (ap -> addr));
			return;
		}
	}
	(*handler) (packet);
}

void dhcpoffer (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;
	struct client_lease *lease, *lp;
	int i;
	int stop_selecting;
	char *name = packet -> packet_type ? "DHCPOFFER" : "BOOTREPLY";
	struct iaddrlist *ap;
	struct option_cache *oc;
	
#ifdef DEBUG_PACKET
	dump_packet (packet);
#endif	

	/* Find a client state that matches the xid... */
	for (client = ip -> client; client; client = client -> next)
		if (client -> xid == packet -> raw -> xid)
			break;

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (!client ||
	    client -> state != S_SELECTING ||
	    (packet -> interface -> hw_address.hlen !=
	     packet -> raw -> hlen) ||
	    (memcmp (packet -> interface -> hw_address.haddr,
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
		log_debug ("%s in wrong transaction.", name);
		return;
	}

	log_info ("%s from %s", name, piaddr (packet -> client_addr));


	/* If this lease doesn't supply the minimum required parameters,
	   blow it off. */
	if (client -> config -> required_options) {
		for (i = 0; client -> config -> required_options [i]; i++) {
			if (!lookup_option
			    (packet -> options.dhcp_hash,
			     client -> config -> required_options [i])) {
				log_info ("%s isn't satisfactory.", name);
				return;
			}
		}
	}

	/* If we've already seen this lease, don't record it again. */
	for (lease = client -> offered_leases; lease; lease = lease -> next) {
		if (lease -> address.len == sizeof packet -> raw -> yiaddr &&
		    !memcmp (lease -> address.iabuf,
			     &packet -> raw -> yiaddr, lease -> address.len)) {
			log_debug ("%s already seen.", name);
			return;
		}
	}

	lease = packet_to_lease (packet);
	if (!lease) {
		log_info ("packet_to_lease failed.");
		return;
	}

	/* If this lease was acquired through a BOOTREPLY, record that
	   fact. */
	if (!packet -> options_valid || !packet -> packet_type)
		lease -> is_bootp = 1;

	/* Record the medium under which this lease was offered. */
	lease -> medium = client -> medium;

	/* Figure out when we're supposed to stop selecting. */
	stop_selecting = (client -> first_sending +
			  client -> config -> select_interval);

	/* If this is the lease we asked for, put it at the head of the
	   list, and don't mess with the arp request timeout. */
	if (lease -> address.len == client -> requested_address.len &&
	    !memcmp (lease -> address.iabuf,
		     client -> requested_address.iabuf,
		     client -> requested_address.len)) {
		lease -> next = client -> offered_leases;
		client -> offered_leases = lease;
	} else {
		/* Put the lease at the end of the list. */
		lease -> next = (struct client_lease *)0;
		if (!client -> offered_leases)
			client -> offered_leases = lease;
		else {
			for (lp = client -> offered_leases; lp -> next;
			     lp = lp -> next)
				;
			lp -> next = lease;
		}
	}

	/* If the selecting interval has expired, go immediately to
	   state_selecting().  Otherwise, time out into
	   state_selecting at the select interval. */
	if (stop_selecting <= 0)
		state_selecting (ip);
	else {
		add_timeout (stop_selecting, state_selecting, client);
		cancel_timeout (send_discover, client);
	}
}

/* Allocate a client_lease structure and initialize it from the parameters
   in the specified packet. */

struct client_lease *packet_to_lease (packet)
	struct packet *packet;
{
	struct client_lease *lease;
	int i;
	struct option_cache *oc;
	struct data_string data;

	lease = (struct client_lease *)new_client_lease ("packet_to_lease");

	if (!lease) {
		log_error ("dhcpoffer: no memory to record lease.\n");
		return (struct client_lease *)0;
	}

	memset (lease, 0, sizeof *lease);

	/* Copy the lease options. */
	lease -> options = packet -> options;
	memset (&packet -> options, 0, sizeof packet -> options);

	lease -> address.len = sizeof (packet -> raw -> yiaddr);
	memcpy (lease -> address.iabuf, &packet -> raw -> yiaddr,
		lease -> address.len);

	/* Figure out the overload flag. */
	oc = lookup_option (lease -> options.dhcp_hash,
			    DHO_DHCP_OPTION_OVERLOAD);
	memset (&data, 0, sizeof data);
	if (oc &&
	    evaluate_option_cache (&data, packet, &lease -> options, oc)) {
		if (data.len > 0)
			i = data.data [0];
		else
			i = 0;
		data_string_forget (&data, "packet_to_lease");
	} else
		i = 0;

	/* If the server name was filled out, copy it. */
	if (!(i & 2) && packet -> raw -> sname [0]) {
		int len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < 64; len++)
			if (!packet -> raw -> sname [len])
				break;
		lease -> server_name = dmalloc (len + 1, "packet_to_lease");
		if (!lease -> server_name) {
			log_error ("dhcpoffer: no memory for filename.\n");
			destroy_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> server_name,
				packet -> raw -> sname, len);
			lease -> server_name [len] = 0;
		}
	}

	/* Ditto for the filename. */
	if ((i & 1) && packet -> raw -> file [0]) {
		int len;
		/* Don't count on the NUL terminator. */
		for (len = 0; len < 64; len++)
			if (!packet -> raw -> file [len])
				break;
		lease -> filename = dmalloc (len + 1, "packet_to_lease");
		if (!lease -> filename) {
			log_error ("dhcpoffer: no memory for filename.\n");
			destroy_client_lease (lease);
			return (struct client_lease *)0;
		} else {
			memcpy (lease -> filename,
				packet -> raw -> file, len);
			lease -> filename [len] = 0;
		}
	}
	return lease;
}	

void dhcpnak (packet)
	struct packet *packet;
{
	struct interface_info *ip = packet -> interface;
	struct client_state *client;

	/* Find a client state that matches the xid... */
	for (client = ip -> client; client; client = client -> next)
		if (client -> xid == packet -> raw -> xid)
			break;

	/* If we're not receptive to an offer right now, or if the offer
	   has an unrecognizable transaction id, then just drop it. */
	if (!client ||
	    (packet -> interface -> hw_address.hlen !=
	     packet -> raw -> hlen) ||
	    (memcmp (packet -> interface -> hw_address.haddr,
		     packet -> raw -> chaddr, packet -> raw -> hlen))) {
		log_debug ("DHCPNAK in wrong transaction.");
		return;
	}

	if (client -> state != S_REBOOTING &&
	    client -> state != S_REQUESTING &&
	    client -> state != S_RENEWING &&
	    client -> state != S_REBINDING) {
		log_debug ("DHCPNAK in wrong state.");
		return;
	}

	log_info ("DHCPNAK from %s", piaddr (packet -> client_addr));

	if (!client -> active) {
		log_info ("DHCPNAK with no active lease.\n");
		return;
	}

	destroy_client_lease (client -> active);
	client -> active = (struct client_lease *)0;

	/* Stop sending DHCPREQUEST packets... */
	cancel_timeout (send_request, client);

	client -> state = S_INIT;
	state_init (client);
}

/* Send out a DHCPDISCOVER packet, and set a timeout to send out another
   one after the right interval has expired.  If we don't get an offer by
   the time we reach the panic interval, call the panic function. */

void send_discover (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;
	int interval;
	int increase = 1;

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - client -> first_sending;

	/* If we're past the panic timeout, call the script and tell it
	   we haven't found anything for this interface yet. */
	if (interval > client -> config -> timeout) {
		state_panic (client);
		return;
	}

	/* If we're selecting media, try the whole list before doing
	   the exponential backoff, but if we've already received an
	   offer, stop looping, because we obviously have it right. */
	if (!client -> offered_leases &&
	    client -> config -> media) {
		int fail = 0;
	      again:
		if (client -> medium) {
			client -> medium = client -> medium -> next;
			increase = 0;
		} 
		if (!client -> medium) {
			if (fail)
				log_fatal ("No valid media types for %s!",
				       client -> interface -> name);
			client -> medium =
				client -> config -> media;
			increase = 1;
		}
			
		log_info ("Trying medium \"%s\" %d",
		      client -> medium -> string, increase);
		script_init (client, "MEDIUM", client -> medium);
		if (script_go (client)) {
			goto again;
		}
	}

	/* If we're supposed to increase the interval, do so.  If it's
	   currently zero (i.e., we haven't sent any packets yet), set
	   it to one; otherwise, add to it a random number between
	   zero and two times itself.  On average, this means that it
	   will double with every transmission. */
	if (increase) {
		if (!client -> interval)
			client -> interval =
				client -> config -> initial_interval;
		else {
			client -> interval +=
				((random () >> 2) %
				 (2 * client -> interval));
		}

		/* Don't backoff past cutoff. */
		if (client -> interval >
		    client -> config -> backoff_cutoff)
			client -> interval =
				((client -> config -> backoff_cutoff / 2)
				 + ((random () >> 2) %
				    client -> config -> backoff_cutoff));
	} else if (!client -> interval)
		client -> interval = client -> config -> initial_interval;
		
	/* If the backoff would take us to the panic timeout, just use that
	   as the interval. */
	if (cur_time + client -> interval >
	    client -> first_sending + client -> config -> timeout)
		client -> interval =
			(client -> first_sending +
			 client -> config -> timeout) - cur_time + 1;

	/* Record the number of seconds since we started sending. */
	if (interval < 255)
		client -> packet.secs = interval;
	else
		client -> packet.secs = 255;

	log_info ("DHCPDISCOVER on %s to %s port %d interval %ld",
	      client -> name ? client -> name : client -> interface -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port), client -> interval);

	/* Send out a packet. */
	result = send_packet (client -> interface, (struct packet *)0,
			      &client -> packet,
			      client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);

	add_timeout (cur_time + client -> interval, send_discover, client);
}

/* state_panic gets called if we haven't received any offers in a preset
   amount of time.   When this happens, we try to use existing leases that
   haven't yet expired, and failing that, we call the client script and
   hope it can do something. */

void state_panic (cpp)
	void *cpp;
{
	struct client_state *client = cpp;
	struct client_lease *loop;
	struct client_lease *lp;

	loop = lp = client -> active;

	log_info ("No DHCPOFFERS received.");

	/* We may not have an active lease, but we may have some
	   predefined leases that we can try. */
	if (!client -> active && client -> leases)
		goto activate_next;

	/* Run through the list of leases and see if one can be used. */
	while (client -> active) {
		if (client -> active -> expiry > cur_time) {
			log_info ("Trying recorded lease %s",
			      piaddr (client -> active -> address));
			/* Run the client script with the existing
			   parameters. */
			script_init (client, "TIMEOUT",
				     client -> active -> medium);
			script_write_params (client, "new_", client -> active);
			if (client -> alias)
				script_write_params (client, "alias_",
						     client -> alias);

			/* If the old lease is still good and doesn't
			   yet need renewal, go into BOUND state and
			   timeout at the renewal time. */
			if (!script_go (client)) {
				if (cur_time <
				    client -> active -> renewal) {
					client -> state = S_BOUND;
					log_info ("bound: renewal in %d seconds.",
					      client -> active -> renewal
					      - cur_time);
					add_timeout ((client ->
						      active -> renewal),
						     state_bound, client);
				} else {
					client -> state = S_BOUND;
					log_info ("bound: immediate renewal.");
					state_bound (client);
				}
				reinitialize_interfaces ();
				go_daemon ();
				return;
			}
		}

		/* If there are no other leases, give up. */
		if (!client -> leases) {
			client -> leases = client -> active;
			client -> active = (struct client_lease *)0;
			break;
		}

	activate_next:
		/* Otherwise, put the active lease at the end of the
		   lease list, and try another lease.. */
		for (lp = client -> leases; lp -> next; lp = lp -> next)
			;
		lp -> next = client -> active;
		if (lp -> next) {
			lp -> next -> next = (struct client_lease *)0;
		}
		client -> active = client -> leases;
		client -> leases = client -> leases -> next;

		/* If we already tried this lease, we've exhausted the
		   set of leases, so we might as well give up for
		   now. */
		if (client -> active == loop)
			break;
		else if (!loop)
			loop = client -> active;
	}

	/* No leases were available, or what was available didn't work, so
	   tell the shell script that we failed to allocate an address,
	   and try again later. */
	log_info ("No working leases in persistent database - sleeping.\n");
	script_init (client, "FAIL", (struct string_list *)0);
	if (client -> alias)
		script_write_params (client, "alias_", client -> alias);
	script_go (client);
	client -> state = S_INIT;
	add_timeout (cur_time + client -> config -> retry_interval,
		     state_init, client);
	go_daemon ();
}

void send_request (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;
	int interval;
	struct sockaddr_in destination;
	struct in_addr from;

	/* Figure out how long it's been since we started transmitting. */
	interval = cur_time - client -> first_sending;

	/* If we're in the INIT-REBOOT or REQUESTING state and we're
	   past the reboot timeout, go to INIT and see if we can
	   DISCOVER an address... */
	/* XXX In the INIT-REBOOT state, if we don't get an ACK, it
	   means either that we're on a network with no DHCP server,
	   or that our server is down.  In the latter case, assuming
	   that there is a backup DHCP server, DHCPDISCOVER will get
	   us a new address, but we could also have successfully
	   reused our old address.  In the former case, we're hosed
	   anyway.  This is not a win-prone situation. */
	if ((client -> state == S_REBOOTING ||
	     client -> state == S_REQUESTING) &&
	    interval > client -> config -> reboot_timeout) {
	cancel:
		client -> state = S_INIT;
		cancel_timeout (send_request, client);
		state_init (client);
		return;
	}

	/* If we're in the reboot state, make sure the media is set up
	   correctly. */
	if (client -> state == S_REBOOTING &&
	    !client -> medium &&
	    client -> active -> medium ) {
		script_init (client, "MEDIUM", client -> active -> medium);

		/* If the medium we chose won't fly, go to INIT state. */
		if (script_go (client))
			goto cancel;

		/* Record the medium. */
		client -> medium = client -> active -> medium;
	}

	/* If the lease has expired, relinquish the address and go back
	   to the INIT state. */
	if (client -> state != S_REQUESTING &&
	    cur_time > client -> active -> expiry) {
		/* Run the client script with the new parameters. */
		script_init (client, "EXPIRE", (struct string_list *)0);
		script_write_params (client, "old_", client -> active);
		if (client -> alias)
			script_write_params (client, "alias_",
					     client -> alias);
		script_go (client);

		/* Now do a preinit on the interface so that we can
		   discover a new address. */
		script_init (client, "PREINIT", (struct string_list *)0);
		if (client -> alias)
			script_write_params (client, "alias_",
					     client -> alias);
		script_go (client);

		client -> state = S_INIT;
		state_init (client);
		return;
	}

	/* Do the exponential backoff... */
	if (!client -> interval)
		client -> interval = client -> config -> initial_interval;
	else {
		client -> interval += ((random () >> 2) %
				       (2 * client -> interval));
	}
	
	/* Don't backoff past cutoff. */
	if (client -> interval >
	    client -> config -> backoff_cutoff)
		client -> interval =
			((client -> config -> backoff_cutoff / 2)
			 + ((random () >> 2) % client -> interval));

	/* If the backoff would take us to the expiry time, just set the
	   timeout to the expiry time. */
	if (client -> state != S_REQUESTING &&
	    cur_time + client -> interval > client -> active -> expiry)
		client -> interval =
			client -> active -> expiry - cur_time + 1;

	/* If the lease T2 time has elapsed, or if we're not yet bound,
	   broadcast the DHCPREQUEST rather than unicasting. */
	if (client -> state == S_REQUESTING ||
	    client -> state == S_REBOOTING ||
	    cur_time > client -> active -> rebind)
		destination.sin_addr.s_addr = INADDR_BROADCAST;
	else
		memcpy (&destination.sin_addr.s_addr,
			client -> destination.iabuf,
			sizeof destination.sin_addr.s_addr);
	destination.sin_port = remote_port;
	destination.sin_family = AF_INET;
#ifdef HAVE_SA_LEN
	destination.sin_len = sizeof destination;
#endif

	if (client -> state == S_RENEWING ||
	    client -> state == S_REBINDING)
		memcpy (&from, client -> active -> address.iabuf,
			sizeof from);
	else
		from.s_addr = INADDR_ANY;

	/* Record the number of seconds since we started sending. */
	if (interval < 255)
		client -> packet.secs = interval;
	else
		client -> packet.secs = 255;

	log_info ("DHCPREQUEST on %s to %s port %d",
	      client -> name ? client -> name : client -> interface -> name,
	      inet_ntoa (destination.sin_addr),
	      ntohs (destination.sin_port));

	if (destination.sin_addr.s_addr != INADDR_BROADCAST &&
	    fallback_interface)
		result = send_packet (fallback_interface,
				      (struct packet *)0,
				      &client -> packet,
				      client -> packet_length,
				      from, &destination,
				      (struct hardware *)0);
	else
		/* Send out a packet. */
		result = send_packet (client -> interface, (struct packet *)0,
				      &client -> packet,
				      client -> packet_length,
				      from, &destination,
				      (struct hardware *)0);

	add_timeout (cur_time + client -> interval,
		     send_request, client);
}

void send_decline (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;

	log_info ("DHCPDECLINE on %s to %s port %d",
	      client -> name ? client -> name : client -> interface -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port));

	/* Send out a packet. */
	result = send_packet (client -> interface, (struct packet *)0,
			      &client -> packet,
			      client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);
}

void send_release (cpp)
	void *cpp;
{
	struct client_state *client = cpp;

	int result;

	log_info ("DHCPRELEASE on %s to %s port %d",
	      client -> name ? client -> name : client -> interface -> name,
	      inet_ntoa (sockaddr_broadcast.sin_addr),
	      ntohs (sockaddr_broadcast.sin_port));

	/* Send out a packet. */
	result = send_packet (client -> interface, (struct packet *)0,
			      &client -> packet,
			      client -> packet_length,
			      inaddr_any, &sockaddr_broadcast,
			      (struct hardware *)0);
}

void make_client_options (client, lease, type, sid, rip, prl,
			  options)
	struct client_state *client;
	struct client_lease *lease;
	u_int8_t *type;
	struct option_cache *sid;
	struct iaddr *rip;
	u_int32_t *prl;
	struct option_state *options;
{
	int i;
	struct option_cache *oc;
	struct buffer *bp = (struct buffer *)0;

	memset (options, 0, sizeof *options);

	/* Send the server identifier if provided. */
	if (sid)
		save_option (options -> dhcp_hash, sid);

	oc = (struct option_cache *)0;

	/* Send the requested address if provided. */
	if (rip) {
		client -> requested_address = *rip;
		if (!(make_const_option_cache
		      (&oc, (struct buffer **)0,
		       rip -> iabuf, rip -> len,
		       &dhcp_options [DHO_DHCP_REQUESTED_ADDRESS],
		       "make_client_options")))
			log_error ("can't make requested address option cache.");
		else {
			save_option (options -> dhcp_hash, oc);
			option_cache_dereference (&oc, "make_client_options");
		}
	} else {
		client -> requested_address.len = 0;
	}

	if (!(make_const_option_cache
	      (&oc, (struct buffer **)0,
	       type, 1, &dhcp_options [DHO_DHCP_MESSAGE_TYPE],
	       "make_client_options")))
		log_error ("can't make message type.");
	else {
		save_option (options -> dhcp_hash, oc);
		option_cache_dereference (&oc, "make_client_options");
	}

	if (prl) {
		/* Figure out how many parameters were requested. */
		for (i = 0; prl [i]; i++)
			;
		if (!buffer_allocate (&bp, i, "make_client_options"))
			log_error ("can't make buffer for parameter request list.");
		else {
			for (i = 0; prl [i]; i++)
				bp -> data [i] = prl [i];
			if (!(make_const_option_cache
			      (&oc, &bp, (u_int8_t *)0, i,
			       &dhcp_options [DHO_DHCP_PARAMETER_REQUEST_LIST],
			       "make_client_options")))
				log_error ("can't make option cache");
			else {
				save_option (options -> dhcp_hash, oc);
				option_cache_dereference
					(&oc, "make_client_options");
			}
		}
	}

	if (!(oc = lookup_option (options -> dhcp_hash,
				  DHO_DHCP_LEASE_TIME))) {
		if (!buffer_allocate (&bp, sizeof (u_int32_t),
				      "make_client_options"))
			log_error ("can't make buffer for requested lease time.");
		else {
			putULong (bp -> data,
				  client -> config -> requested_lease);
			if (!(make_const_option_cache
			      (&oc, &bp, (u_int8_t *)0, sizeof (u_int32_t),
			       &dhcp_options [DHO_DHCP_LEASE_TIME],
			       "make_client_options")))
				log_error ("can't make option cache");
			else {
				save_option (options -> dhcp_hash, oc);
				option_cache_dereference
					(&oc, "make_client_options");
			}
		}
	}		
	/* oc = (struct option_cache *)0; (we'd need this if we were
	   				   going to use oc again */

	/* Run statements that need to be run on transmission. */
	if (client -> config -> on_transmission)
		execute_statements_in_scope
			((struct packet *)0, &lease -> options, options,
			 client -> config -> on_transmission,
			 (struct group *)0);
}

void make_discover (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	struct dhcp_packet *raw;
	unsigned char discover = DHCPDISCOVER;
	int i;
	struct option_state options;

	memset (&client -> packet, 0, sizeof (client -> packet));

	make_client_options (client,
			     lease, &discover, (struct option_cache *)0,
			     lease ? &lease -> address : (struct iaddr *)0,
			     client -> config -> requested_options,
			     &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet, 0,
			      &options, (struct agent_options *)0,
			      0, 0, 0, (struct data_string *)0);
	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.htype;
	client -> packet.hlen = client -> interface -> hw_address.hlen;
	client -> packet.hops = 0;
	client -> packet.xid = random ();
	client -> packet.secs = 0; /* filled in by send_discover. */
	memset (&(client -> packet.ciaddr),
		0, sizeof client -> packet.ciaddr);
	memset (&(client -> packet.yiaddr),
		0, sizeof client -> packet.yiaddr);
	memset (&(client -> packet.siaddr),
		0, sizeof client -> packet.siaddr);
	memset (&(client -> packet.giaddr),
		0, sizeof client -> packet.giaddr);
	memcpy (client -> packet.chaddr,
		client -> interface -> hw_address.haddr,
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)client -> packet,
		  sendpkt->packet_length);
#endif
}


void make_request (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char request = DHCPREQUEST;
	int i, j;
	unsigned char *tmp, *digest;
	unsigned char *old_digest_loc;
	struct option_state options;
	struct option_cache *oc;

	memset (&client -> packet, 0, sizeof (client -> packet));

	if (client -> state == S_REQUESTING)
		oc = lookup_option (lease -> options.dhcp_hash,
				    DHO_DHCP_SERVER_IDENTIFIER);
	else
		oc = (struct option_cache *)0;

	make_client_options (client, lease, &request, oc,
			     ((client -> state == S_REQUESTING ||
			       client -> state == S_REBOOTING)
			      ? &lease -> address
			      : (struct iaddr *)0),
			     client -> config -> requested_options,
			     &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet, 0,
			      &options, (struct agent_options *)0,
			      0, 0, 0, (struct data_string *)0);
	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.htype;
	client -> packet.hlen = client -> interface -> hw_address.hlen;
	client -> packet.hops = 0;
	client -> packet.xid = client -> xid;
	client -> packet.secs = 0; /* Filled in by send_request. */
	if (can_receive_unicast_unconfigured (client -> interface))
		client -> packet.flags = 0;
	else
		client -> packet.flags = htons (BOOTP_BROADCAST);

	/* If we own the address we're requesting, put it in ciaddr;
	   otherwise set ciaddr to zero. */
	if (client -> state == S_BOUND ||
	    client -> state == S_RENEWING ||
	    client -> state == S_REBINDING) {
		memcpy (&client -> packet.ciaddr,
			lease -> address.iabuf, lease -> address.len);
		client -> packet.flags = 0;
	} else {
		memset (&client -> packet.ciaddr, 0,
			sizeof client -> packet.ciaddr);
	}

	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	memset (&client -> packet.giaddr, 0,
		sizeof client -> packet.giaddr);
	memcpy (client -> packet.chaddr,
		client -> interface -> hw_address.haddr,
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)client -> packet, sendpkt->packet_length);
#endif
}

void make_decline (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char decline = DHCPDECLINE;
	int i;
	struct option_cache *oc;

	struct option_state options;

	memset (&client -> packet, 0, sizeof (client -> packet));

	oc = lookup_option (lease -> options.dhcp_hash,
			    DHO_DHCP_SERVER_IDENTIFIER);
	make_client_options (client, lease, &decline, oc,
			     &lease -> address, (u_int32_t *)0,
			     &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet, 0,
			      &options, (struct agent_options *)0,
			      0, 0, 0, (struct data_string *)0);
	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.htype;
	client -> packet.hlen = client -> interface -> hw_address.hlen;
	client -> packet.hops = 0;
	client -> packet.xid = client -> xid;
	client -> packet.secs = 0; /* Filled in by send_request. */
	if (can_receive_unicast_unconfigured (client -> interface))
		client -> packet.flags = 0;
	else
		client -> packet.flags = htons (BOOTP_BROADCAST);

	/* ciaddr must always be zero. */
	memset (&client -> packet.ciaddr, 0,
		sizeof client -> packet.ciaddr);
	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	memset (&client -> packet.giaddr, 0,
		sizeof client -> packet.giaddr);
	memcpy (client -> packet.chaddr,
		client -> interface -> hw_address.haddr,
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)client -> packet, sendpkt->packet_length);
#endif
}

void make_release (client, lease)
	struct client_state *client;
	struct client_lease *lease;
{
	unsigned char request = DHCPRELEASE;
	int i;
	struct option_cache *oc;

	struct option_state options;

	memset (&client -> packet, 0, sizeof (client -> packet));

	oc = lookup_option (lease -> options.dhcp_hash,
			    DHO_DHCP_SERVER_IDENTIFIER);
	make_client_options (client, lease, &request, oc,
			     &lease -> address, (u_int32_t *)0,
			     &options);

	/* Set up the option buffer... */
	client -> packet_length =
		cons_options ((struct packet *)0, &client -> packet, 0,
			      &options, (struct agent_options *)0,
			      0, 0, 0, (struct data_string *)0);
	if (client -> packet_length < BOOTP_MIN_LEN)
		client -> packet_length = BOOTP_MIN_LEN;

	client -> packet.op = BOOTREQUEST;
	client -> packet.htype = client -> interface -> hw_address.htype;
	client -> packet.hlen = client -> interface -> hw_address.hlen;
	client -> packet.hops = 0;
	client -> packet.xid = random ();
	client -> packet.secs = 0;
	client -> packet.flags = 0;
	memcpy (&client -> packet.ciaddr,
		lease -> address.iabuf, lease -> address.len);
	memset (&client -> packet.yiaddr, 0,
		sizeof client -> packet.yiaddr);
	memset (&client -> packet.siaddr, 0,
		sizeof client -> packet.siaddr);
	memset (&client -> packet.giaddr, 0,
		sizeof client -> packet.giaddr);
	memcpy (client -> packet.chaddr,
		client -> interface -> hw_address.haddr,
		client -> interface -> hw_address.hlen);

#ifdef DEBUG_PACKET
	dump_packet (sendpkt);
	dump_raw ((unsigned char *)client -> packet,
		  client -> packet_length);
#endif
}

void destroy_client_lease (lease)
	struct client_lease *lease;
{
	int i;

	if (lease -> server_name)
		dfree (lease -> server_name, "destroy_client_lease");
	if (lease -> filename)
		dfree (lease -> filename, "destroy_client_lease");
	option_state_dereference (&lease -> options);
	free_client_lease (lease, "destroy_client_lease");
}

FILE *leaseFile;

void rewrite_client_leases ()
{
	struct interface_info *ip;
	struct client_state *client;
	struct client_lease *lp;

	if (leaseFile)
		fclose (leaseFile);
	leaseFile = fopen (path_dhclient_db, "w");
	if (!leaseFile)
		log_fatal ("can't create %s: %m", path_dhclient_db);

	/* Write out all the leases attached to configured interfaces that
	   we know about. */
	for (ip = interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			for (lp = client -> leases; lp; lp = lp -> next) {
				write_client_lease (client, lp, 1);
			}
			if (client -> active)
				write_client_lease (client,
						    client -> active, 1);
		}
	}

	/* Write out any leases that are attached to interfaces that aren't
	   currently configured. */
	for (ip = dummy_interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			for (lp = client -> leases; lp; lp = lp -> next) {
				write_client_lease (client, lp, 1);
			}
			if (client -> active)
				write_client_lease (client,
						    client -> active, 1);
		}
	}
	fflush (leaseFile);
}

void write_client_lease (client, lease, rewrite)
	struct client_state *client;
	struct client_lease *lease;
	int rewrite;
{
	int i;
	struct tm *t;
	static int leases_written;
	struct option_cache *oc;
	struct data_string ds;

	if (!rewrite) {
		if (leases_written++ > 20) {
			rewrite_client_leases ();
			leases_written = 0;
		}
	}

	/* If the lease came from the config file, we don't need to stash
	   a copy in the lease database. */
	if (lease -> is_static)
		return;

	if (!leaseFile) {	/* XXX */
		leaseFile = fopen (path_dhclient_db, "w");
		if (!leaseFile)
			log_fatal ("can't create %s: %m", path_dhclient_db);
	}

	fprintf (leaseFile, "lease {\n");
	if (lease -> is_bootp)
		fprintf (leaseFile, "  bootp;\n");
	fprintf (leaseFile, "  interface \"%s\";\n",
		 client -> interface -> name);
	if (client -> name)
		fprintf (leaseFile, "  name \"%s\";\n", client -> name);
	fprintf (leaseFile, "  fixed-address %s;\n",
		 piaddr (lease -> address));
	if (lease -> filename)
		fprintf (leaseFile, "  filename \"%s\";\n",
			 lease -> filename);
	if (lease -> server_name)
		fprintf (leaseFile, "  server-name \"%s\";\n",
			 lease -> filename);
	if (lease -> medium)
		fprintf (leaseFile, "  medium \"%s\";\n",
			 lease -> medium -> string);

	memset (&ds, 0, sizeof ds);
	for (i = 0; i < OPTION_HASH_SIZE; i++) {
		pair p;
		for (p = lease -> options.dhcp_hash [i]; p; p = p -> cdr) {
			oc = (struct option_cache *)p -> car;
			if (evaluate_option_cache (&ds, (struct packet *)0,
						   &lease -> options, oc)) {
				fprintf (leaseFile,
					 "  option %s %s;\n",
					 oc -> option -> name,
					 pretty_print_option
					 (oc -> option -> code,
					  ds.data, ds.len, 1, 1));
				data_string_forget (&ds,
						    "write_client_lease");
			}
		}
	}

	/* Note: the following is not a Y2K bug - it's a Y1.9K bug.   Until
	   somebody invents a time machine, I think we can safely disregard
	   it. */
	t = gmtime (&lease -> renewal);
	fprintf (leaseFile,
		 "  renew %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	t = gmtime (&lease -> rebind);
	fprintf (leaseFile,
		 "  rebind %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	t = gmtime (&lease -> expiry);
	fprintf (leaseFile,
		 "  expire %d %d/%d/%d %02d:%02d:%02d;\n",
		 t -> tm_wday, t -> tm_year + 1900,
		 t -> tm_mon + 1, t -> tm_mday,
		 t -> tm_hour, t -> tm_min, t -> tm_sec);
	fprintf (leaseFile, "}\n");
	fflush (leaseFile);
}

/* Variables holding name of script and file pointer for writing to
   script.   Needless to say, this is not reentrant - only one script
   can be invoked at a time. */
char scriptName [256];
FILE *scriptFile;

void script_init (client, reason, medium)
	struct client_state *client;
	char *reason;
	struct string_list *medium;
{
	int fd;
#ifndef HAVE_MKSTEMP

	do {
#endif
		strcpy (scriptName, "/tmp/dcsXXXXXX");
#ifdef HAVE_MKSTEMP
		fd = mkstemp (scriptName);
#else
		if (!mktemp (scriptName))
			log_fatal ("can't create temporary client script %s: %m",
			       scriptName);
		fd = creat (scriptName, 0600);
	} while (fd < 0);
#endif

	scriptFile = fdopen (fd, "w");
	if (!scriptFile)
		log_fatal ("can't write script file: %m");
	fprintf (scriptFile, "#!/bin/sh\n\n");
	if (client) {
		if (client -> interface) {
			fprintf (scriptFile, "interface=\"%s\"\n",
				 client -> interface -> name);
			fprintf (scriptFile, "export interface\n");
		}
		if (client -> name)
			fprintf (scriptFile, "client=\"%s\"\n",
				 client -> name);
		fprintf (scriptFile, "export client\n");
	}
	if (medium) {
		fprintf (scriptFile, "medium=\"%s\"\n", medium -> string);
		fprintf (scriptFile, "export medium\n");
	}
	fprintf (scriptFile, "reason=\"%s\"\n", reason);
	fprintf (scriptFile, "export reason\n");
}

void script_write_params (client, prefix, lease)
	struct client_state *client;
	char *prefix;
	struct client_lease *lease;
{
	int i;
	struct data_string data;
	struct option_cache *oc;

	fprintf (scriptFile, "%sip_address=\"%s\"\n",
		 prefix, piaddr (lease -> address));
	fprintf (scriptFile, "export %sip_address\n", prefix);

	/* For the benefit of Linux (and operating systems which may
	   have similar needs), compute the network address based on
	   the supplied ip address and netmask, if provided.  Also
	   compute the broadcast address (the host address all ones
	   broadcast address, not the host address all zeroes
	   broadcast address). */

	memset (&data, 0, sizeof data);
	oc = lookup_option (lease -> options.dhcp_hash, DHO_SUBNET_MASK);
	if (oc && evaluate_option_cache (&data, (struct packet *)0,
					 &lease -> options, oc)) {
		if (data.len > 3) {
			struct iaddr netmask, subnet, broadcast;

			memcpy (netmask.iabuf, data.data, data.len);
			netmask.len = data.len;
			data_string_forget (&data, "script_write_params");

			subnet = subnet_number (lease -> address, netmask);
			if (subnet.len) {
				fprintf (scriptFile,
					 "%snetwork_number=\"%s\";\n",
					 prefix, piaddr (subnet));
				fprintf (scriptFile,
					 "export %snetwork_number\n", prefix);

				oc = lookup_option (lease -> options.dhcp_hash,
						    DHO_BROADCAST_ADDRESS);
				if (!oc ||
				    !evaluate_option_cache (&data,
							    (struct packet *)0,
							    &lease -> options,
							    oc)) {
					broadcast = broadcast_addr (subnet,
								    netmask);
					if (broadcast.len) {
						fprintf (scriptFile,
							 "%s%s=\"%s\";\n",
							 prefix,
							 "broadcast_address",
							 piaddr (broadcast));
						fprintf (scriptFile,
							 "export %s%s\n",
							 prefix,
							 "broadcast_address");
					}
				}
			}
		}
		data_string_forget (&data, "script_write_params");
	}

	if (lease -> filename) {
		fprintf (scriptFile, "%sfilename=\"%s\";\n",
			 prefix, lease -> filename);
		fprintf (scriptFile, "export %sfilename\n", prefix);
	}
	if (lease -> server_name) {
		fprintf (scriptFile, "%sserver_name=\"%s\";\n",
			 prefix, lease -> server_name);
		fprintf (scriptFile, "export %sserver_name\n", prefix);
	}

	execute_statements_in_scope ((struct packet *)0, &lease -> options,
				     &lease -> options,
				     client -> config -> on_receipt,
				     (struct group *)0);

	for (i = 0; i < OPTION_HASH_SIZE; i++) {
		pair hp;

		for (hp = lease -> options.dhcp_hash [i]; hp; hp = hp -> cdr) {
			oc = (struct option_cache *)hp -> car;

			if (evaluate_option_cache (&data, (struct packet *)0,
						   &lease -> options, oc)) {

				if (data.len) {
					char *s = (dhcp_option_ev_name
						   (oc -> option));
				
					fprintf (scriptFile,
						 "%s%s=\"%s\"\n", prefix, s,
						 (pretty_print_option
						  (oc -> option -> code,
						   data.data, data.len,
						   0, 0)));
					fprintf (scriptFile,
						 "export %s%s\n", prefix, s);
				}
				data_string_forget (&data,
						    "script_write_params");
			}
		}
	}
	fprintf (scriptFile, "%sexpiry=\"%d\"\n",
		 prefix, (int)lease -> expiry); /* XXX */
	fprintf (scriptFile, "export %sexpiry\n", prefix);
}

int script_go (client)
	struct client_state *client;
{
	int rval;

	if (client)
		fprintf (scriptFile, "%s\n",
			 client -> config -> script_name);
	else
		fprintf (scriptFile, "%s\n",
			 top_level_config.script_name);
	fprintf (scriptFile, "exit $?\n");
	fclose (scriptFile);
	chmod (scriptName, 0700);
	rval = system (scriptName);	
	if (!save_scripts)
		unlink (scriptName);
	return rval;
}

char *dhcp_option_ev_name (option)
	struct option *option;
{
	static char evbuf [256];
	int i;

	if (strlen (option -> name) + 1 > sizeof evbuf)
		log_fatal ("option %s name is larger than static buffer.");
	for (i = 0; option -> name [i]; i++) {
		if (option -> name [i] == '-')
			evbuf [i] = '_';
		else
			evbuf [i] = option -> name [i];
	}

	evbuf [i] = 0;
	return evbuf;
}

void go_daemon ()
{
	static int state = 0;
	int pid;

	/* Don't become a daemon if the user requested otherwise. */
	if (no_daemon) {
		write_client_pid_file ();
		return;
	}

	/* Only do it once. */
	if (state)
		return;
	state = 1;

	/* Stop logging to stderr... */
	log_perror = 0;

	/* Become a daemon... */
	if ((pid = fork ()) < 0)
		log_fatal ("Can't fork daemon: %m");
	else if (pid)
		exit (0);
	/* Become session leader and get pid... */
	pid = setsid ();

	/* Close standard I/O descriptors. */
        close(0);
        close(1);
        close(2);

	write_client_pid_file ();
}

void write_client_pid_file ()
{
	FILE *pf;
	int pfdesc;

	pfdesc = open (path_dhclient_pid, O_CREAT | O_TRUNC | O_WRONLY, 0644);

	if (pfdesc < 0) {
		log_error ("Can't create %s: %m", path_dhclient_pid);
		return;
	}

	pf = fdopen (pfdesc, "w");
	if (!pf)
		log_error ("Can't fdopen %s: %m", path_dhclient_pid);
	else {
		fprintf (pf, "%ld\n", (long)getpid ());
		fclose (pf);
	}
}

void client_location_changed ()
{
	struct interface_info *ip;
	struct client_state *client;

	for (ip = interfaces; ip; ip = ip -> next) {
		for (client = ip -> client; client; client = client -> next) {
			switch (client -> state) {
			      case S_SELECTING:
				cancel_timeout (send_discover, client);
				break;

			      case S_BOUND:
				cancel_timeout (state_bound, client);
				break;

			      case S_REBOOTING:
			      case S_REQUESTING:
			      case S_RENEWING:
				cancel_timeout (send_request, client);
				break;

			      case S_INIT:
			      case S_REBINDING:
				break;
			}
			client -> state = S_INIT;
			state_reboot (client);
		}
	}
}
