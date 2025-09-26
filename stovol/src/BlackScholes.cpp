#include "BlackScholes.h"
#include <assert.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
// Removed dependency on src/Utils.h (not available in stovol build)

using namespace std;

double CND(double x) {
  double x1 = fabs(x);
  double n = (1.0 / sqrt(2 * M_PI)) * exp(-pow(x1, 2) / 2);
  double k = 1.0 / (1 + 0.2316419 * x1);
  double n1 = 1 - n * (+0.319381530 * k - 0.356563782 * pow(k, 2) + 1.781477937 * pow(k, 3) -
                       1.821255978 * pow(k, 4) + 1.330274429 * pow(k, 5));

  if (x < 0) {
    return 1 - n1;
  }

  return n1;
}

double BSCall(double S, double K, double R, double V, double T) {
  double bscall = INFINITY;
  if (V <= 0) {
    // do nothing
  } else {
    double d = V * std::sqrt(T);
    double d1 = (log(S / K) + (V * V / 2) * T) / d;
    double d2 = d1 - d;

    // 交易部採 S * exp(-R * T) * CND(d1) - K * exp(-R * T) * CND(d2);
    // 結算部採 exp(-@r*@t) * (@s * N(@d1) - @k * N(@d2))
    bscall = exp(-R * T) * (S * CND(d1) - K * CND(d2));
  }

  return bscall;
}

double BSPut(double S, double K, double R, double V, double T) {
  double bsput = INFINITY;
  if (V <= 0) {
    // do nothing
  } else {
    double d = V * std::sqrt(T);
    double d1 = (log(S / K) + (V * V / 2) * T) / d;
    double d2 = d1 - d;

    // 交易部採 K * exp(-R * T) * CND(-d2) - S * exp(-R * T) * CND(-d1);   // BlackScholes
    // 結算部 exp(-@r*@t) * (@k * N(-@d2) - @s * N(-@d1)) // Black
    bsput = exp(-R * T) * (K * CND(-d2) - S * CND(-d1));
  }

  return bsput;
}

double BSCallVega_v2(double S, double K, double R, double V, double T) {
  double callvega = INFINITY;
  if (V <= 0) {
    // do nothing
  } else {
    double s1 = S;
    // 交易部採 (log(S / K) + (V * V / 2) * T) / (V * std::sqrt(T));
    // 結算部 (Log(@s1/@k) + (@r+@v^2/2) * @t) / (@v*@t^0.5)
    double d1 = (log(S / K) + (R + V * V / 2.0) * T) / (V * std::sqrt(T));
    double n1 = 1.0 / sqrt(2 * M_PI) * exp(-(d1 * d1) / 2);
    callvega = n1 * s1 * sqrt(T);
  }

  return callvega;
}

#define BSPutVega_v2(S, K, R, V, T) BSCallVega_v2(S, K, R, V, T)

double NTPutIV(double S, double K, double R, double T, double p, int *iter) {
  const int maxIteration = 200;
  double v0 = 1.0;
  double tolerance = 1E-5;
  double p0 = INFINITY;
  int cnt = 0;
  do {
    if (v0 <= 0.0) {
      v0 = 0.0;
      break;
    }
    p0 = BSPut(S, K, R, v0, T) - p;
    double vega = BSPutVega_v2(S, K, R, v0, T);
    if (vega == 0.0)
      v0 = 0.0;
    else
      v0 = v0 - p0 / vega;
  } while (fabs(p0) > tolerance && cnt++ < maxIteration);

  if (cnt > maxIteration) {
    v0 = 0.0;
  }

  if (v0 < 0.0 || v0 > 9999.0) {
    v0 = 0.0;
  }

  if (iter != NULL) {
    *iter = cnt;
  }

  return v0;
}

double NTCallIV(double S, double K, double R, double T, double p, int *iter) {
  const int maxIteration = 200;
  double v0 = 1.0;
  double tolerance = 1E-5;
  double p0 = INFINITY;
  int cnt = 0;
  do {
    if (v0 <= 0.0) {
      v0 = 0.0;
      break;
    }
    p0 = BSCall(S, K, R, v0, T) - p;
    double vega = BSCallVega_v2(S, K, R, v0, T);
    if (vega == 0.0)
      v0 = 0.0;
    else
      v0 = v0 - p0 / vega;
  } while (fabs(p0) > tolerance && cnt++ < maxIteration);

  if (cnt > maxIteration) {
    v0 = 0.0;
  }

  if (v0 < 0.0 || v0 > 9999.0) {
    v0 = 0.0;
  }

  if (iter != NULL) {
    *iter = cnt;
  }

  return v0;
}

double DebugBSPut(double S, double K, double R, double V, double T, std::stringstream &ss) {
  double bsput = INFINITY;
  if (V <= 0) {
    // do nothing
  } else {
    double d = V * std::sqrt(T);
    double d1 = (log(S / K) + (V * V / 2) * T) / d;
    double d2 = d1 - d;
    ss.setf(std::ios::fixed);
    ss << setprecision(6);
    ss << "d=" << d;
    ss << " d1=" << d1;
    ss << " d2=" << d2;

    // 交易部採 K * exp(-R * T) * CND(-d2) - S * exp(-R * T) * CND(-d1);
    // 結算部 exp(-@r*@t) * (@k * N(-@d2) - @s * N(-@d1))
    bsput = exp(-R * T) * (K * CND(-d2) - S * CND(-d1));
    ss << " bsput=exp(-R*T)*(K*CND(-d2)-S*CND(-d1))=" << bsput << endl;
  }

  return bsput;
}

double DebugBSPutVega_v2(double S, double K, double R, double V, double T, std::stringstream &ss) {
  double callvega = INFINITY;
  if (V <= 0) {
    // do nothing
  } else {
    double s1 = S;
    // 交易部採 (log(S / K) + (V * V / 2) * T) / (V * std::sqrt(T));
    // 結算部 (Log(@s1/@k) + (@r+@v^2/2) * @t) / (@v*@t^0.5)
    double d1 = (log(S / K) + (R + V * V / 2.0) * T) / (V * std::sqrt(T));
    double n1 = 1.0 / sqrt(2 * M_PI) * exp(-(d1 * d1) / 2);
    ss.setf(std::ios::fixed);
    ss << setprecision(6);
    ss << "s1=" << s1;
    ss << " d1=" << d1;
    ss << " n1=" << n1;

    callvega = n1 * s1 * sqrt(T);
    ss << " vega=n1*s1*sqrt(T)=" << callvega << endl;
  }

  return callvega;
}

double DebugNTPutIV(double S, double K, double R, double T, double p, std::stringstream &ss) {
  const int maxIteration = 200;
  double v0 = 1.0;
  double tolerance = 1E-5;
  double p0 = INFINITY;
  int cnt = 0;
  do {
    if (v0 <= 0.0) {
      v0 = 0.0;
      break;
    }
    ss.setf(std::ios::fixed);
    ss << setprecision(6);
    ss << "BSPut(" << S << "," << K << "," << R << "," << v0 << "," << T << ")" << endl;
    ss << "BSPut: ";
    p0 = DebugBSPut(S, K, R, v0, T, ss) - p;
    ss << "p0 = BSPut - " << p << " = " << p0 << endl;

    ss << "BSPutVega_v2: ";
    double vega = DebugBSPutVega_v2(S, K, R, v0, T, ss);
    // ss << Utils::Format("vega = BSPutVega_v2(%.6f, %.6f, %.6f, %.6f,
    // %.6f) = %.6f",
    //                     S, K, R, v0, T, vega) << endl;

    ss << "v0 = " << v0 << " - " << p0 << " / BSPutVega = ";
    if (vega != 0.0)
      ss << (v0 - p0 / vega) << endl;
    else
      ss << "NA (vega=0)" << endl;
    if (vega == 0.0)
      v0 = 0.0;
    else
      v0 = v0 - p0 / vega;
  } while (fabs(p0) > tolerance && cnt++ < maxIteration);

  ss << "v0=" << v0 << " iter=" << cnt << endl;

  if (cnt > maxIteration) {
    v0 = 0.0;
  }

  if (v0 < 0.0 || v0 > 9999.0) {
    v0 = 0.0;
  }

  return v0;
}
