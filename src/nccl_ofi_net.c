/*
 * Copyright (c) 2018-2024 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Copyright (c) 2015-2018, NVIDIA CORPORATION. All rights reserved.
 */

#include "config.h"

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <ctype.h>

#include "nccl_ofi.h"
#include "nccl_ofi_param.h"
#include "tracepoint.h"
#if HAVE_CUDA
#include "nccl_ofi_cuda.h"
#endif
#include "nccl_ofi_sendrecv.h"
#include "nccl_ofi_rdma.h"
#include "nccl_ofi_topo.h"
#include "nccl_ofi_math.h"
#include "nccl_ofi_idpool.h"

/* Indicates if GPUDirect is supported by libfabric provider */
enum gdr_support_level_t support_gdr = GDR_UNKNOWN;

/* Indicates if the cudaDeviceFlushGPUDirectRDMAWrites function should be used
 * to flush data to the GPU. Note, CUDA flush support is not supported on all
 * platforms and should be disabled by default */
bool cuda_flush = false;

/* number of duplicate providers to create for each discovered
 * provider, including renaming to cause NCCL to create additional
 * rings to use the connections
 */
int nic_dup_conns = 0;

/* number of cq entries to read in a single call to fi_cq_read.
   This variable will be updated during init (hence, can not be
   const), but will not change during execution.  Therefore, it may be
   read in the polling loop without protection of a lock. */
size_t cq_read_count = 1;

const char *provider_filter = NULL;

/* Indicates if memory registration of local buffers is required */
bool local_mr = false;
/* Indicates if endpoint memory registration is required */
bool endpoint_mr = false;

/* Indicates if remote virtual addressing is used */
bool virt_addr_mr = false;

/* Selected communication protocol. */
const char *nccl_ofi_selected_protocol = "SENDRECV";

/* Internode network latency. */
float net_latency = .0;

/* Size of a memory page */
long system_page_size = -1;

/*
 * @brief	Allocate memory region for memory registration
 *
 * This function allocates memory that covers full page aligned.
 *
 * Internally allocated memory that is registered is required to cover
 * full memory pages. For more information, see functions
 * `register_internal_mr_buffers()` and `reg_internal_mr_ep()`.
 *
 * To free deallocate the memory region, function
 * nccl_net_ofi_dealloc_mr_buffer() must be used.
 *
 * @param	size
 *		Size of the memory region. Must be a multiple of system memory page size.
 * @return	Pointer to memory region. Memory region is aligned to system memory page size.
 * @return	0, on success
 *		error, on others
 */
int nccl_net_ofi_alloc_mr_buffer(size_t size, void **ptr)
{
	assert(system_page_size > 0);
	assert(NCCL_OFI_IS_ALIGNED(size, system_page_size));

	*ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (OFI_UNLIKELY(*ptr == MAP_FAILED)) {
		NCCL_OFI_WARN("Unable to map MR buffer (%d %s)",
			      errno, strerror(errno));
		*ptr = NULL;
		return -errno;
	}
	assert(NCCL_OFI_IS_PTR_ALIGNED(*ptr, system_page_size));
	return 0;
}

/*
 * @brief	Deallocate memory region allocated by function nccl_net_ofi_alloc_mr_buffer()
 *
 * @return	Pointer to memory region
 * @param	size
 *		Size of the memory region
 * @return	0, on success
 *		error, on others
 */
int nccl_net_ofi_dealloc_mr_buffer(void *ptr, size_t size)
{
	int ret = 0;

	assert(NCCL_OFI_IS_PTR_ALIGNED(ptr, system_page_size));
	assert(NCCL_OFI_IS_ALIGNED(size, system_page_size));

	ret = munmap(ptr, size);
	if (OFI_UNLIKELY(ret != 0)) {
		NCCL_OFI_WARN("Unable to unmap MR buffer (%d %s)",
			      errno, strerror(errno));
		ret = -errno;
	}
	return ret;
}


int nccl_net_ofi_create_plugin(nccl_net_ofi_plugin_t **plugin_p)
{
	int ret = 0;
	const char *provider_filter = NULL;

	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Initializing " PACKAGE_STRING);

	/* Print Libfabric version */
	uint32_t fab_version = fi_version();
	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Using Libfabric version %u.%u", FI_MAJOR(fab_version),
			FI_MINOR(fab_version));

	system_page_size = sysconf(_SC_PAGESIZE);
	if (OFI_UNLIKELY(system_page_size == -1)) {
		NCCL_OFI_WARN("Failed to get system page size (%d %s)", errno, strerror(errno));
		ret = -ENOTSUP;
		goto exit;
	}
	assert(NCCL_OFI_IS_POWER_OF_TWO(system_page_size));
	assert(system_page_size > 0);

#if HAVE_CUDA
	ret = nccl_net_ofi_cuda_init();
	if (ret != 0) {
		NCCL_OFI_WARN("CUDA initialization failed.");
		goto exit;
	}

	int cuda_version;
	if (nccl_net_ofi_cuDriverGetVersion(&cuda_version) != CUDA_SUCCESS) {
		NCCL_OFI_WARN("cuDriverGetVersion failed");
		ret = -ENOTSUP;
		goto exit;
	}

	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Using CUDA driver version %d", cuda_version);
	if (ofi_nccl_cuda_flush_enable()) {
		if (nccl_net_ofi_cuFlushGPUDirectRDMAWrites == NULL) {
			NCCL_OFI_WARN("CUDA flush requested, but cuFlushGPUDirectRDMAWrites not found.");
			cuda_flush = false;
		} else {
			NCCL_OFI_WARN("CUDA flush enabled");
			cuda_flush = true;
		}
	}
#endif

	/* configuration parameters */
	nic_dup_conns = ofi_nccl_nic_dup_conns();
	net_latency = (float)ofi_nccl_net_latency();
	cq_read_count = ofi_nccl_cq_read_count();

	if (platform_init) {
		ret = platform_init(&provider_filter);
		if (ret != 0)
			goto exit;
	}

	/* Select and initialize protocol data structure.
	 * platform_init() may change the default, so this must occur
	 * after the platform init call.
	 */
	if (ofi_nccl_protocol()) {
		nccl_ofi_selected_protocol = ofi_nccl_protocol();
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Using transport protocol %s (user set)",
			      nccl_ofi_selected_protocol);
	} else {
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Using transport protocol %s",
			      nccl_ofi_selected_protocol);
	}

	if (0 == strcasecmp(nccl_ofi_selected_protocol, "SENDRECV")) {
		ret = nccl_net_ofi_sendrecv_init(provider_filter, plugin_p);
		if (ret != 0) {
			NCCL_OFI_WARN("Failed to initialize sendrecv protocol");
			goto exit;
		}
	} else if (0 == strcasecmp(nccl_ofi_selected_protocol, "RDMA")) {
		ret = nccl_net_ofi_rdma_init(provider_filter, plugin_p);
		if (ret != 0) {
			NCCL_OFI_WARN("Failed to initialize rdma protocol");
			goto exit;
		}
	} else {
		NCCL_OFI_WARN("Unable to find plugin protocol %s", nccl_ofi_selected_protocol);
		ret = -ENOTSUP;
		goto exit;
	}

	/* In order to set endpoint options and potentially NCCL configuration
	 * options (such as NCCL_PROTO) during the plugin initialization
	 * process, we need to create an endpoint and call the platform hook
	 * "platform_config_endpoint" using "get_ep". This code makes the
	 * assumption that the thread calling "nccl_net_ofi_init" will make
	 * communication calls. As well, since without this code the endpoint
	 * would be created the first time "get_ep" in called during a listen or
	 * connect call, creating the endpoint earlier would not be a waste of
	 * resources. This initialization happens once per process, and thus it
	 * does not matter which device is used to create the endpoint.
	 */
	int dev_id = 0;
	nccl_net_ofi_device_t *base_dev = (*plugin_p)->devs[dev_id];
	nccl_net_ofi_ep_t *base_ep = NULL;

	ret = (*plugin_p)->devs[dev_id]->get_ep(base_dev, &base_ep);
	if (ret != 0) {
		goto exit;
	}
	ret = base_ep->release_ep(base_ep);
	if (ret != 0) {
		goto exit;
	}

	assert(support_gdr != GDR_UNKNOWN);

	/* we don't actually know if GDR is supported until we've
	 * created the first endpoint, so this check needs to be way
	 * down here
	 */
	if (nic_dup_conns > 0 && support_gdr != GDR_UNSUPPORTED) {
		NCCL_OFI_WARN("NCCL_OFI_NIC_DUP_CONNS set on platform that supports GPUDirect RDMA.  This configuration is not supported.");
		ret = -ENOTSUP;
		goto exit;
	}

 exit:
	if (ret != 0) {
		NCCL_OFI_WARN(PACKAGE_NAME " initialization failed");
	}
	return ret;
}

static int get_device_pci_path(struct fid_nic *nic_info, char** path)
{
	int ret = 0;
	struct fi_pci_attr *pci = NULL;
	char *device_path = NULL;

	if (nic_info->bus_attr->bus_type != FI_BUS_PCI) {
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET,
			      "Invalid type of PCI bus returned %d",
			      nic_info->bus_attr->bus_type);
		ret = -EINVAL;;
		goto exit;
	}

	pci = &nic_info->bus_attr->attr.pci;
	ret = asprintf(&device_path,
		       "/sys/class/pci_bus/%04x:%02x/../../%04x:%02x:%02x.%01x",
		       pci->domain_id, pci->bus_id,
		       pci->domain_id, pci->bus_id, pci->device_id, pci->function_id);
	if (ret < 0) {
		NCCL_OFI_WARN("pciPath: Allocation failure");
		ret = -ENOMEM;
		goto exit;
	} else {
		ret = 0;
	}

	*path = realpath(device_path, NULL);
	if (*path == NULL) {
		NCCL_OFI_WARN("pciPath: Could not find real path of %s",
			      device_path);
		ret = -errno;
		goto exit;
	}

 exit:
	if (device_path)
		free(device_path);

	return ret;
}

/*
 * @brief	Set default properties for libfabric NIC info.
 */
static int set_nic_props_default(int dev_id, struct fi_info *nic_prov,
				 nccl_ofi_properties_t *props)
{
	props->name = strdup(nic_prov->domain_attr->name);

	/*
	 * Currently, libfabric providers provide multiple `fi_info`
	 * objects for devices with multiple ports. So, safely assume port number
	 * to be always 1.
	 */
	props->port_number = 1;
	props->max_communicators = 0;
	props->guid = dev_id;

	props->latency = net_latency >= .0 ? net_latency : .0;

	/*
	 * Maximum number of grouped receives. Currently, we set it to 1 to
	 * maintain single send/recv semantics (similar to NCCL versions < v2.12).
	 *
	 * Grouped receives are useful for alltoall collectives where one
	 * receiver is expected to receive from multiple remote GPUs using
	 * PXN(PCIe X NVLINK) feature. Other collectives like allreduce aren't
	 * impacted with this feature as NCCL doesn't aggregate receives from
	 * same source.
	 */
	props->max_group_receives = NCCL_OFI_MAX_RECVS;

	if (support_gdr == GDR_SUPPORTED) {
		props->hmem_support = true;
	} else {
		props->hmem_support = false;
	}

	props->dmabuf_support = false;

	/* Should be successful for ptrSupport invocation */
	return 0;
}

/*
 * @brief	Set properties obtained from libfabric NIC Info.
 *
 * @return	Populated props structure
 */
int nccl_net_ofi_info_properties(struct fi_info *nic_prov, int dev_id, int num_devices,
				 nccl_ofi_properties_t *props)
{
	int ret = 0;
	nccl_ofi_properties_t dev_props = {0};
	struct fid_nic *nic_info = NULL;

	ret = set_nic_props_default(dev_id, nic_prov, &dev_props);
	if (ret != 0)
		goto error;

	/* Change default values as set by NIC attributes */
	nic_info = (struct fid_nic *)nic_prov->nic;
	if (nic_info == NULL) {
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET,
			      "No NIC info for dev %d. Supplying default values for NIC properties.",
			      dev_id);
		ret = 0;
		goto exit;
	}

	/* name is NULL if device is a part of multirail config */
	/* overriding default name only if value is available from provider */
	if (nic_info->device_attr->name) {
		dev_props.name = strdup(nic_info->device_attr->name);
	}

	/*
	 * Determine the scope of MRs for providers to report global
	 * registration support to NCCL
	 */
	if (nic_prov->domain_attr->mr_mode & FI_MR_ENDPOINT) {
		dev_props.mr_scope = NCCL_OFI_MR_SCOPE_ENDPOINT;
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Libfabric provider associates MRs with endpoints");
	} else {
		dev_props.mr_scope = NCCL_OFI_MR_SCOPE_DOMAIN;
		NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Libfabric provider associates MRs with domains");
	}

	/* Speed reported in Mbps */
	dev_props.port_speed = nic_info->link_attr->speed / (1e6);

	ret = get_device_pci_path(nic_info, &(dev_props.pci_path));
	if (ret != 0) {
		ret = 0;
		props->pci_path = NULL;
	}

	if (nic_dup_conns > 1) {
#if HAVE_CUDA
		int num_gpus_visible, active_cuda_device, gpus_per_conn;
		size_t c;

		if (nccl_net_ofi_cuDeviceGetCount(&num_gpus_visible) != CUDA_SUCCESS) {
			NCCL_OFI_WARN("Error getting CUDA device count");
			ret = -ENOTSUP;
			goto error;
		}

		if (nccl_net_ofi_cuCtxGetDevice(&active_cuda_device) != CUDA_SUCCESS) {
			NCCL_OFI_WARN("Error getting current CUDA device");
			ret = -ENOTSUP;
			goto error;
		}

		gpus_per_conn = num_gpus_visible / num_devices;
		if (gpus_per_conn == 0) gpus_per_conn = 1;

		/* The goal is to have gpus_per_conn gpus in the local
		 * system think that any given virtual nic is the one
		 * that they should use, and that it is different than
		 * the other NICs in the system.  We do this by only
		 * leaving a valid device id in pci_path when
		 * active_cuda_device / gpus_per_comm is equal to the
		 * NIC dev index we're currently querying.  For the
		 * others, we provide a PCIPath that points at the PCI
		 * Bus itself, which NCCL will interpret to be on a
		 * different complex than the bus (assuming the NIC
		 * BUS and GPU BUS are the same).
		 *
		 * There are a bunch of assumptions in this logic,
		 * such that the physical NICs in the system don't
		 * have PCI affinity with the GPUs.  Given that we've
		 * already established that GPUDirect doesn't work,
		 * this is probably ok; any affinity is lost by
		 * bouncing through host buffers anyway.
		 */
		if (active_cuda_device / gpus_per_conn  != dev_id) {
			for (c=strlen(dev_props.pci_path); c && dev_props.pci_path[c] != '/'; c--) {
				dev_props.pci_path[c] = '\0';
			}
			dev_props.pci_path[c] = '\0';
		}
		NCCL_OFI_TRACE(NCCL_INIT, "Returning synthetic PCI path for device %d of  %s",
			       dev_id, dev_props.pci_path);

		snprintf(dev_props.name, FI_NAME_MAX + 2, "%s-%x", nic_info->device_attr->name, dev_id);
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Adjusted dev %d device name to %s",
			       dev_id, dev_props.name);
#else
		NCCL_OFI_WARN("NIC_DUP_CONNS enabled on platform that does not support NIC_DUP_CONNS.  This should not happen.");
		ret = -ENOTSUP;
		goto error;
#endif
	}

	goto exit;

 error:
	props = NULL;
 exit:
	*props = dev_props;
	return ret;
}


int nccl_net_ofi_reg_mr_dma_buf_send_comm(nccl_net_ofi_send_comm_t *send_comm,
					  void *data, size_t size,
					  int type, uint64_t offset, int fd,
					  nccl_net_ofi_mr_handle_t **handle)
{
	return -ENOTSUP;
}

int nccl_net_ofi_reg_mr_dma_buf_recv_comm(nccl_net_ofi_recv_comm_t *recv_comm,
					  void *data, size_t size,
					  int type, uint64_t offset, int fd,
					  nccl_net_ofi_mr_handle_t **handle)
{
	return -ENOTSUP;
}


int nccl_net_ofi_query_provider_capabilities(struct fi_info *selected_provider,
					     unsigned int num_providers)
{
	NCCL_OFI_INFO(NCCL_INIT | NCCL_NET, "Selected Provider is %s (found %d nics)",
		      selected_provider->fabric_attr->prov_name, num_providers);

	/* Prior to Libfabric 1.18.0, there was no way to disable
	 * Libfabric from making CUDA calls.  While the EFA path was
	 * CUDA clean, it could use the shm provider, which did make
	 * CUDA calls.  Rather than muck with side channel ways of
	 * disabling CUDA in old Libfabric, just require newer
	 * Libfabric. */
	if (strncmp("efa", selected_provider->fabric_attr->prov_name, strlen("efa")) == 0) {
		if (FI_VERSION_LT(fi_version(), FI_VERSION(1, 18))) {
			NCCL_OFI_WARN("EFA provider requires at least libfabric version 1.18.0.");
			return -ENOTSUP;
		}
	}

	/* Check if provider requires local memory registration */
	if (selected_provider->domain_attr->mr_mode & FI_MR_LOCAL) {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s requires registration of local memory buffers",
			       selected_provider->fabric_attr->prov_name);
		local_mr = true;
	} else {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s does not require registration of local memory buffers",
			       selected_provider->fabric_attr->prov_name);
		local_mr = false;
	}

	/* Check if provider uses remote virtual addressing */
	if (selected_provider->domain_attr->mr_mode & FI_MR_VIRT_ADDR) {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s uses remote virtual addressing",
			       selected_provider->fabric_attr->prov_name);
		virt_addr_mr = true;
	} else {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s does not use remote virtual addressing",
			       selected_provider->fabric_attr->prov_name);
		virt_addr_mr = false;
	}

	/* Check if provider uses endpoint memory registration */
	if (selected_provider->domain_attr->mr_mode & FI_MR_ENDPOINT) {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s requires endpoint memory registration",
			       selected_provider->fabric_attr->prov_name);
		endpoint_mr = true;
	} else {
		NCCL_OFI_TRACE(NCCL_INIT | NCCL_NET, "Provider %s does not require endpoint memory registration",
			       selected_provider->fabric_attr->prov_name);
		endpoint_mr = false;
	}

	return 0;
}
