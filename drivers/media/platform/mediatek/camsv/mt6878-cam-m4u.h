/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT6878_CAM_M4U_H__
#define __MT6878_CAM_M4U_H__

#include <linux/scatterlist.h>
#include <linux/types.h>

int mt6878_cam_m4u_map_sg(unsigned long iova, struct sg_table *sgt, size_t *size);
void mt6878_cam_m4u_unmap(unsigned long iova, size_t size);
phys_addr_t mt6878_cam_m4u_iova_to_phys(unsigned long iova);

#endif
