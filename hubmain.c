
#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

#define MAX_PORTS 16

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

struct mbuf_table {
	uint16_t len;
	struct rte_mbuf *m_table[BURST_SIZE];
};

struct port_tx_conf_s {
	uint64_t tx_tsc;
	struct mbuf_table tx_mbufs[MAX_PORTS];
} __rte_cache_aligned;
static struct port_tx_conf_s port_tx_conf;

/* hubmain.c: Basic hub operation. Packet recieved on a port
 * will be flooded in other ports.
 * Developed from Basic DPDK skeleton forwarding example.
 */

static struct rte_mempool *mbuf_pool, *header_pool;

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;

	if (port >= rte_eth_dev_count())
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/* Send burst of packets on an output interface */
static void
send_burst(uint8_t port)
{
	struct rte_mbuf **m_table;
	uint16_t n;
	int ret;

	m_table = (struct rte_mbuf **)port_tx_conf.tx_mbufs[port].m_table;
	n = port_tx_conf.tx_mbufs[port].len;


	ret = rte_eth_tx_burst(port, 0, m_table, n);
#if 0
	printf("ramsp: %s: current len %d port %d ret %d\n", __FUNCTION__, n, port, ret);
	rte_pktmbuf_dump(stdout, m_table[0], 16);
#endif
	while (unlikely (ret < n)) {
		rte_pktmbuf_free(m_table[ret]);
		ret++;
	}

	port_tx_conf.tx_mbufs[port].len = 0;
}

/* Send burst of outgoing packet, if timeout expires. */
static inline void
send_timeout_burst(void)
{
	uint64_t cur_tsc;
	uint8_t port;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	cur_tsc = rte_rdtsc();
	if (likely (cur_tsc < port_tx_conf.tx_tsc + drain_tsc))
		return;

	for (port = 0; port < MAX_PORTS; port++) {
		if (port_tx_conf.tx_mbufs[port].len != 0)
			send_burst(port);
	}
	port_tx_conf.tx_tsc = cur_tsc;
}

/* Queue the packets for each port */
static inline void
flood_send_pkt(struct rte_mbuf *pkt, uint8_t port)
{
	uint16_t len;

	/* Put new packet into the output queue */
	len = port_tx_conf.tx_mbufs[port].len;
	port_tx_conf.tx_mbufs[port].m_table[len] = pkt;
	port_tx_conf.tx_mbufs[port].len = ++len;

	/* Transmit packets */
	if (unlikely(BURST_SIZE == len))
		send_burst(port);
}

/* flood forward of the input packet */
static inline void
flood_forward(struct rte_mbuf *m, uint8_t rx_port, uint8_t nb_ports)
{
	uint8_t port;
	struct rte_mbuf *pkt;

	/* Mark all packet's segments as referenced port_num times */
	rte_pktmbuf_refcnt_update(m, (uint16_t)nb_ports);

	for (port = 0; port < nb_ports; port++) {
		/* skip for own port */
		if (unlikely (port == rx_port))
			continue; 

		pkt = m;		
		flood_send_pkt(pkt, port);
	}
	rte_pktmbuf_free(m);
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and flooding in other ports.
 */
static __attribute__((noreturn)) void
lcore_main(void)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;
	int i;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	for (port = 0; port < nb_ports; port++)
		if (rte_eth_dev_socket_id(port) > 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/*
		 * Receive packets on a port will be flooded in other ports
		 */
		for (port = 0; port < nb_ports; port++) {
			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0))
				continue;

			for (i = 0; i < nb_rx; i++) {
			/*	rte_pktmbuf_dump(stdout, bufs[i], 16); */
				flood_forward(bufs[i], port, nb_ports);
			}
		}
		/* Send out packets from TX queues */
		send_timeout_burst();
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	unsigned nb_ports;
	uint8_t portid;

	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Creates a new mempool in memory to hold the headers for cloned ports. */
	header_pool = rte_pktmbuf_pool_create("header_pool", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_PKTMBUF_HEADROOM * 2, rte_socket_id());

	if (header_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init header mbuf pool\n");

	/* Initialize all ports. */
	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the master core only. */
	lcore_main();

	return 0;
}
