#pragma once

#include <vector>
#include <cstdint>

// The code utiized here was adapted
// from GNU Radio, Spiral and Phil Karn's
// FEC library
namespace viterbi
{
    typedef union
    {
        // decision_t is a BIT vector
        unsigned char *t;
        unsigned int *w;
        unsigned short *s;
        unsigned char *c;
    } decision_t;

    typedef union
    {
        unsigned char *t;
    } metric_t;

    struct v
    {
        std::vector<unsigned char> metrics;
        metric_t old_metrics, new_metrics, metrics1, metrics2; /* Pointers to path metrics, swapped on every bit */
        std::vector<unsigned char> decisions;
    };
}
