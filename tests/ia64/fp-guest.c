/* SPDX-License-Identifier: GPL-2.0-or-later */
long sd(long a, long b)
{
    return a / b;
}
unsigned long ud(unsigned long a, unsigned long b)
{
    return a / b;
}
long sm(long a, long b)
{
    return a % b;
}
unsigned long um(unsigned long a, unsigned long b)
{
    return a % b;
}
int sd32(int a, int b)
{
    return a / b;
}
unsigned ud32(unsigned a, unsigned b)
{
    return a / b;
}
int sm32(int a, int b)
{
    return a % b;
}
unsigned um32(unsigned a, unsigned b)
{
    return a % b;
}
double dd(double a, double b)
{
    return a / b;
}
float fd(float a, float b)
{
    return a / b;
}
double sq(double a)
{
    return __builtin_sqrt(a);
}
float fsq(float a)
{
    return __builtin_sqrtf(a);
}
double si(long a)
{
    return a;
}
double ui(unsigned long a)
{
    return a;
}
float sif(long a)
{
    return a;
}
float uif(unsigned long a)
{
    return a;
}
long is(double a)
{
    return a;
}
unsigned long iu(double a)
{
    return a;
}
