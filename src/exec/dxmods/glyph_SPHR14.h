/***********************************************************************/
/* Open Visualization Data Explorer                                    */
/* (C) Copyright IBM Corp. 1989,1999                                   */
/* ALL RIGHTS RESERVED                                                 */
/* This code licensed under the                                        */
/*    "IBM PUBLIC LICENSE - Open Visualization Data Explorer"          */
/***********************************************************************/
#define SPHR14PTS 14 
#define SPHR14TRS 24 
static Point points[SPHR14PTS] = {
 {  0.0000000000F,     -1.0000000000F,      0.0000000000F },
 {  0.8660253882F,     -0.5000000000F,      0.0000000000F },
 {  0.4330126643F,     -0.5000000000F,      0.7500000000F },
 { -0.4330127537F,     -0.5000000000F,      0.7499999404F },
 { -0.8660253882F,     -0.5000000000F,     -0.0000000757F },
 { -0.4330126047F,     -0.5000000000F,     -0.7500000596F },
 {  0.4330126345F,     -0.5000000000F,     -0.7500000596F },
 {  0.8660253882F,      0.5000000000F,      0.0000000000F },
 {  0.4330126643F,      0.5000000000F,      0.7500000000F },
 { -0.4330127537F,      0.5000000000F,      0.7499999404F },
 { -0.8660253882F,      0.5000000000F,     -0.0000000757F },
 { -0.4330126047F,      0.5000000000F,     -0.7500000596F },
 {  0.4330126345F,      0.5000000000F,     -0.7500000596F },
 {  0.0000000000F,      1.0000000000F,      0.0000000000F }
};

#include <dxconfig.h>


static Point normals[SPHR14PTS] = {
 {  0.0000000000F,     -1.0000000000F,      0.0000000000F },
 {  0.8660253882F,     -0.5000000000F,      0.0000000000F },
 {  0.4330126643F,     -0.5000000000F,      0.7500000000F },
 { -0.4330127537F,     -0.5000000000F,      0.7499999404F },
 { -0.8660253882F,     -0.5000000000F,     -0.0000000757F },
 { -0.4330126047F,     -0.5000000000F,     -0.7500000596F },
 {  0.4330126345F,     -0.5000000000F,     -0.7500000596F },
 {  0.8660253882F,      0.5000000000F,      0.0000000000F },
 {  0.4330126643F,      0.5000000000F,      0.7500000000F },
 { -0.4330127537F,      0.5000000000F,      0.7499999404F },
 { -0.8660253882F,      0.5000000000F,     -0.0000000757F },
 { -0.4330126047F,      0.5000000000F,     -0.7500000596F },
 {  0.4330126345F,      0.5000000000F,     -0.7500000596F },
 {  0.0000000000F,      1.0000000000F,      0.0000000000F }
};

static Triangle triangles[] = {
 {   0,     1,     2 },
 {   0,     2,     3 },
 {   0,     3,     4 },
 {   0,     4,     5 },
 {   0,     5,     6 },
 {   0,     6,     1 },
 {   1,     8,     2 },
 {   1,     7,     8 },
 {   2,     8,     3 },
 {   3,     8,     9 },
 {   3,    10,     4 },
 {   3,     9,    10 },
 {   4,    10,     5 },
 {   5,    10,    11 },
 {   5,    12,     6 },
 {   5,    11,    12 },
 {   6,    12,     1 },
 {   1,    12,     7 },
 {   7,    13,     8 },
 {   8,    13,     9 },
 {   9,    13,    10 },
 {  10,    13,    11 },
 {  11,    13,    12 },
 {  12,    13,     7 }
};