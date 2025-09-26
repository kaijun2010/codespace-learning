#ifndef BLACKSCHOLES_H
#define BLACKSCHOLES_H

#include <cstdlib>
#include <sstream>

//   S = targetF (標的期貨價格)
//   K = m_strike_price (履約價)
//   R = m_p03_compound (利率)
//   T = m_remaining_days / OCF_MOCF_DAY_COUNT(g_ocf_mocf_day_count)
//   P = price (OPT.EM)
double CND(double x);
double BSCall(double S, double K, double R, double V, double T);
double BSPut(double S, double K, double R, double V, double T);
double NTPutIV(double S, double K, double R, double T, double p, int *iter = NULL);
double NTCallIV(double S, double K, double R, double T, double p, int *iter = NULL);

double DebugNTPutIV(double S, double K, double R, double T, double p, std::stringstream &ss);

#endif
