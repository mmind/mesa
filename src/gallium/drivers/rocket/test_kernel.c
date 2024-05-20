#include <xf86drm.h>
#include "drm-uapi/rocket_drm.h"

unsigned uint64_t regcmd_blob[] = {
};

unsigned uint64_t bias_blob[] = {
};

unsigned uint64_t input_blob[] = {
};

unsigned uint64_t weights_blob[] = {
};

#define OUTPUT_SIZE = 1000000;

static uint64_t create_bo(int device_fd, unsigned size, uint64_t *phys_addr)
{
    struct drm_rocket_create_bo create_bo = {0,};
    struct drm_rocket_mmap_bo mem_map = {0,};
    int ret;

    create_bo.size = size;

    ret = drmIoctl(device_fd, DRM_IOCTL_ROCKET_CREATE_BO, &create_bo);
    assert(ret >= 0);

    mem_map.handle = create_bo.handle;

    ret = drmIoctl(device_fd, DRM_IOCTL_ROCKET_MMAP_BO, &mem_map);
    assert(ret >= 0);

    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd, mem_map.offset);
    assert(map != MAP_FAILED);

    *phys_addr = create_bo.dma_address;

    return map;
}

static void patch(uint64_t *regcmd, unsigned register, uint64_t phys_addr)
{
    for (int i = 0; i < sizeof(regcmd_blob); i++) {
        target = regcmd[i] >> 48;
        value = (regcmd[i] >> 16) & 0x0000000011111111;
        reg = regcmd[i] & 0x0000000000001111;

        if (reg == register) {
            uint64_t packed_value = 0;
            packed_value = ((uint64_t) target) << 48;
            packed_value |= ((uint64_t) value) << 16;
            packed_value |= (uint64_t) register;

            regcmd[i] = packed_value;
        }
    }
}

static uint64_t create_regcmd(int device_fd, uint8_t **output_out)
{
    uint64_t regcmd_phys;
    uint64_t bias_phys;
    unsigned regcmd_size = sizeof(regcmd_blob) * 8;
    uint64_t *regcmd = create_bo(device_fd, regcmd_size, &regcmd_phys);
    uint8_t *output, *bias;

    memcpy(regcmd, regcmd_blob, regcmd_size);

    input = create_bo(device_fd, sizeof(input_blob), &input_phys);
    patch(regcmd, REG_CNA_FEATURE_DATA_ADDR, input_phys);

    weights = create_bo(device_fd, sizeof(weights_blob), &weights_phys);
    patch(regcmd, REG_CNA_DCOMP_ADDR0, weights_phys);

    output = create_bo(device_fd, OUTPUT_SIZE, &output_phys);
    patch(regcmd, REG_DPU_DST_BASE_ADDR, output_phys);

    bias = create_bo(device_fd, sizeof(bias_blob), &bias_phys);
    patch(regcmd, REG_DPU_RDMA_RDMA_BS_BASE_ADDR, bias_phys);

    // TODO: Sync to device?

    *output_out = output;

    return regcmd_phys;
}

int main (int argc, char **argv)
{
    int device_fd;
    unint8_t *output;
    int ret;
 
    device_fd = open("/dev/accel/accel0", O_RDWR);

    regcmd_phys_address = create_regcmd(device_fd, &output);

    struct rocket_submit submit = {
        .regcmd = regcmd_phys_address,
    };

    ret = drmIoctl(screen->fd, DRM_IOCTL_ROCKET_SUBMIT, &submit);

    // TODO: Sync from device?

    // TODO: Check output matches

    return ret;
}