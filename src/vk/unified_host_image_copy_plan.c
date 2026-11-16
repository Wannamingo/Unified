// SPDX-License-Identifier: MIT
#include "unified_host_image_copy_plan.h"
#include <stddef.h>
#include <string.h>
static bool plus(uint64_t a,uint64_t b,uint64_t*out){if(a>UINT64_MAX-b)return false;*out=a+b;return true;}
static bool times(uint64_t a,uint64_t b,uint64_t*out){if(a&&b>UINT64_MAX/a)return false;*out=a*b;return true;}
static uint32_t blocks(uint32_t n,uint32_t b){return n/b+(n%b!=0);}
bool unified_host_image_plan(const struct unified_host_level*l,const struct unified_host_texel_region*r,uint8_t*mapping,uint64_t available,uint64_t image_offset,struct unified_host_copy_plan*out){
 if(!l||!r||!out||!mapping||!available||!l->width||!l->height||!l->depth||!l->layers||!l->block_width||!l->block_height||!l->block_bytes||l->block_bytes>16)return false;
 if(l->tile_side!=0&&l->tile_side!=4&&l->tile_side!=16)return false;
 if(r->x<0||r->y<0||r->z<0||!r->width||!r->height||!r->depth||!r->layer_count)return false;
 const uint32_t x=(uint32_t)r->x,y=(uint32_t)r->y,z=(uint32_t)r->z;
 if((uint64_t)x+r->width>l->width||(uint64_t)y+r->height>l->height||(uint64_t)z+r->depth>l->depth)return false;
 if(x%l->block_width||y%l->block_height)return false;
 // A partial final block is legal only at the actual mip boundary.
 if((r->width%l->block_width&&(uint64_t)x+r->width!=l->width)||(r->height%l->block_height&&(uint64_t)y+r->height!=l->height))return false;
 if(l->image_3d){if(l->layers!=1||r->base_layer!=0||r->layer_count!=1)return false;}
 else if(l->depth!=1||z||r->depth!=1||(uint64_t)r->base_layer+r->layer_count>l->layers)return false;
 if((r->memory_row_length&&(r->memory_row_length<r->width||r->memory_row_length%l->block_width))||(r->memory_image_height&&(r->memory_image_height<r->height||r->memory_image_height%l->block_height)))return false;
 struct unified_host_copy_plan p;memset(&p,0,sizeof(p));
 p.plane.mapping=mapping;p.plane.available=available;
 if(!plus(image_offset,l->offset,&p.plane.offset))return false;
 p.plane.row_stride=l->row_stride;p.plane.surface_stride=l->image_3d?l->surface_stride:l->array_stride;
 p.plane.width_blocks=blocks(l->width,l->block_width);p.plane.height_blocks=blocks(l->height,l->block_height);
 p.plane.depth_slices=l->image_3d?l->depth:l->layers;p.plane.block_bytes=l->block_bytes;p.plane.tile_side=l->tile_side;
 p.blocks=(struct unified_host_region){x/l->block_width,y/l->block_height,l->image_3d?z:r->base_layer,blocks(r->width,l->block_width),blocks(r->height,l->block_height),l->image_3d?r->depth:r->layer_count};
 uint32_t row_blocks=blocks(r->memory_row_length?r->memory_row_length:r->width,l->block_width);
 uint32_t surface_rows=blocks(r->memory_image_height?r->memory_image_height:r->height,l->block_height);
 if(!times(row_blocks,l->block_bytes,&p.host_row_stride)||!times(p.host_row_stride,surface_rows,&p.host_surface_stride))return false;
 uint64_t span,row_tail,surface_tail;
 if(!times(p.blocks.depth-1,p.host_surface_stride,&surface_tail)||!times(p.blocks.height-1,p.host_row_stride,&row_tail)||!times(p.blocks.width,l->block_bytes,&span)||!plus(span,row_tail,&span)||!plus(span,surface_tail,&span)||span>SIZE_MAX)return false;
 p.host_span=span;
 // Check layout bounds before publishing a plan. This mirrors size math only;
 // it does not invoke the copy helper or dereference either mapping.
 uint64_t image_row,rows=l->tile_side?blocks(p.plane.height_blocks,l->tile_side):p.plane.height_blocks;
 if(l->tile_side){if(!times(blocks(p.plane.width_blocks,l->tile_side),(uint64_t)l->tile_side*l->tile_side*l->block_bytes,&image_row))return false;}
 else if(!times(p.plane.width_blocks,l->block_bytes,&image_row))return false;
 uint64_t surface,end;
 if(p.plane.row_stride<image_row||!times(rows-1,p.plane.row_stride,&surface)||!plus(surface,image_row,&surface)||p.plane.surface_stride<surface||!times(p.plane.depth_slices-1,p.plane.surface_stride,&end)||!plus(end,surface,&end)||!plus(end,p.plane.offset,&end)||end>available||end>SIZE_MAX)return false;
 if((uintptr_t)mapping>UINTPTR_MAX-end)return false;
 *out=p;return true;
}
