#include <arm_neon.h>

/* { dg-do compile } */
/* { dg-skip-if "" { *-*-* } { "-fno-fat-lto-objects" } } */
/* { dg-excess-errors "" { xfail arm*-*-* } } */
/* { dg-skip-if "" { arm*-*-* } } */

void
f_vst3q_lane_p8 (poly8_t * p, poly8x16x3_t v)
{
  /* { dg-error "lane 16 out of range 0 - 15" "" { xfail arm*-*-* } 0 } */
  vst3q_lane_p8 (p, v, 16);
  /* { dg-error "lane -1 out of range 0 - 15" "" { xfail arm*-*-* } 0 } */
  vst3q_lane_p8 (p, v, -1);
  return;
}
