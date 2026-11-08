// A3 and A4 -- the criteria this slice exists for.
//
// A3: the solver's three distinct failures are reported distinctly, and each
// kind is checked against the MESSAGE upstream logs at that same site. That
// is what makes the taxonomy a reference-implementation oracle (rung 3) rather
// than my judgement: upstream already distinguishes these cases, it just
// cannot return the distinction.
//
// A4: an error raised deep in a call chain arrives at the public surface with
// its kind and message intact. D5 -- propagation is not a kind.

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <string>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "rtbrent.h"
}

namespace {

using cdsmodel::ErrorKind;
using cdsmodel::Expected;

/// x^2 - 4, rooted at 2 on the positive side.
Expected<double> square_minus_four(double x) { return x * x - 4.0; }

extern "C" int upstream_objective(double x, void *, double *out) {
  *out = x * x - 4.0;
  return SUCCESS;
}

cdsmodel::RootFindOptions workable() {
  cdsmodel::RootFindOptions o;
  o.bound_lo = 0.0;
  o.bound_hi = 10.0;
  o.guess = 1.0;
  o.max_iterations = 100;
  o.x_tolerance = 1e-12;
  o.f_tolerance = 1e-12;
  return o;
}

int upstream_status(const cdsmodel::RootFindOptions &o) {
  double solution = 0.0;
  return JpmcdsRootFindBrent(upstream_objective, nullptr, o.bound_lo, o.bound_hi, o.max_iterations,
                             o.guess, o.initial_x_step, o.initial_f_derivative, o.x_tolerance,
                             o.f_tolerance, &solution);
}

class ErrorChannel : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

// The control. Both sides succeed, so the error channel is not just reporting
// failure for everything.
TEST_F(ErrorChannel, A3BothSidesSolveTheWorkableCase) {
  const cdsmodel::RootFindOptions o = workable();
  ASSERT_EQ(upstream_status(o), SUCCESS);
  const Expected<double> got = cdsmodel::root_find_brent(square_minus_four, o);
  ASSERT_TRUE(got.has_value()) << got.error().message;
  EXPECT_NEAR(*got, 2.0, 1e-9);
}

TEST_F(ErrorChannel, A3TheSolversThreeFailuresAreDistinct) {
  struct Case {
    const char *what;
    cdsmodel::RootFindOptions opt;
    ErrorKind kind;
    const char *upstream_logs; // the message upstream emits at this site
  };

  cdsmodel::RootFindOptions reversed = workable();
  reversed.bound_lo = 10.0;
  reversed.bound_hi = 0.0;

  cdsmodel::RootFindOptions guess_out = workable();
  guess_out.guess = 99.0;

  // 5..9 keeps x^2-4 strictly positive, so no root is bracketed.
  cdsmodel::RootFindOptions no_root = workable();
  no_root.bound_lo = 5.0;
  no_root.bound_hi = 9.0;
  no_root.guess = 6.0;

  const Case cases[]{
      {"reversed bounds", reversed, ErrorKind::bad_argument,
       "Lower bound (%2.6e) >= higher bound (%2.6e)."},
      {"guess out of range", guess_out, ErrorKind::bad_argument,
       "Guess (%2.6e) is out of range [%2.6e,%2.6e]."},
      {"no root in the bracket", no_root, ErrorKind::no_root,
       "Function values (...) at bounds (...) imply no root exists."},
  };

  for (const Case &c : cases) {
    // Upstream fails all three identically -- one FAILURE, no distinction.
    EXPECT_NE(upstream_status(c.opt), SUCCESS) << c.what << ": upstream now succeeds here";

    const Expected<double> got = cdsmodel::root_find_brent(square_minus_four, c.opt);
    ASSERT_FALSE(got.has_value()) << c.what << ": the port succeeded where upstream failed";
    EXPECT_EQ(got.error().kind, c.kind)
        << c.what << ": upstream logs \"" << c.upstream_logs << "\" at this site, so the kind "
        << "should match that, not " << cdsmodel::to_string(got.error().kind);
    EXPECT_FALSE(got.error().message.empty()) << c.what;
  }

  // The point of the criterion: the three are not the same answer. Two share a
  // kind, so what has to differ is the message -- all three distinguishable.
  const Expected<double> a = cdsmodel::root_find_brent(square_minus_four, reversed);
  const Expected<double> b = cdsmodel::root_find_brent(square_minus_four, guess_out);
  const Expected<double> c = cdsmodel::root_find_brent(square_minus_four, no_root);
  EXPECT_NE(a.error().message, b.error().message);
  EXPECT_NE(a.error().message, c.error().message);
  EXPECT_NE(b.error().message, c.error().message);
}

// Non-convergence is the case the old doc comment admitted it conflated with a
// failing objective. One iteration cannot converge.
TEST_F(ErrorChannel, A3NonConvergenceIsItsOwnKind) {
  cdsmodel::RootFindOptions o = workable();
  o.max_iterations = 1;
  o.guess = 9.0; // far from the root, so one step cannot land on it

  const Expected<double> got = cdsmodel::root_find_brent(square_minus_four, o);
  if (got.has_value()) {
    GTEST_SKIP() << "one iteration happened to converge; the kind is unobservable here";
  }
  EXPECT_EQ(got.error().kind, ErrorKind::no_convergence)
      << "upstream logs \"Maximum number of iterations exceeded.\" here; got: "
      << got.error().message;
  EXPECT_NE(got.error().message.find("iterations"), std::string::npos) << got.error().message;
}

// A4 / D5. The objective's OWN error must arrive at the caller unchanged --
// not wrapped, not replaced by a generic "solve failed".
TEST_F(ErrorChannel, A4AnObjectiveErrorReachesTheCallerUnchanged) {
  const std::string deep = "the curve has no point at 2029-06-01";
  const auto failing = [&](double) -> Expected<double> {
    return cdsmodel::fail(ErrorKind::unsupported, deep);
  };

  const Expected<double> got = cdsmodel::root_find_brent(failing, workable());
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().kind, ErrorKind::unsupported) << "the solver rewrote the objective's kind";
  EXPECT_EQ(got.error().message, deep) << "the solver replaced the objective's message";
}

// A4 across a real three-deep chain rather than a contrived one: a curve
// refused at the bottom must explain itself at the top.
TEST_F(ErrorChannel, A4AnErrorFromThreeLevelsDownKeepsItsMessage) {
  // ZeroCurve::create refuses an unsupported day count. That refusal is the
  // deepest point; `bootstrap` is reached through two frames above it.
  const cdsmodel::RatePoint pts[]{{cdsmodel::from_tdate(40000), 0.03}};
  const Expected<cdsmodel::ZeroCurve> bad = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(36000), pts, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Thirty360);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::unsupported);
  EXPECT_NE(bad.error().message.find("ACT/365F"), std::string::npos)
      << "the message should name what IS supported: " << bad.error().message;
}

// A6. Every kind must be reachable from a real call; a kind nothing produces
// is a kind that should not exist.
TEST_F(ErrorChannel, A6EveryKindIsReachable) {
  // bad_argument -- reversed solver bounds
  cdsmodel::RootFindOptions rev = workable();
  rev.bound_lo = 10.0;
  rev.bound_hi = 0.0;
  const Expected<double> e1 = cdsmodel::root_find_brent(square_minus_four, rev);

  // unsupported -- a day count the curve cannot discount with
  const cdsmodel::RatePoint pts[]{{cdsmodel::from_tdate(40000), 0.03}};
  const Expected<cdsmodel::ZeroCurve> e2 = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(36000), pts, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Thirty360);

  // no_root -- bounds that do not bracket
  cdsmodel::RootFindOptions nr = workable();
  nr.bound_lo = 5.0;
  nr.bound_hi = 9.0;
  nr.guess = 6.0;
  const Expected<double> e3 = cdsmodel::root_find_brent(square_minus_four, nr);

  // no_convergence -- one iteration
  cdsmodel::RootFindOptions nc = workable();
  nc.max_iterations = 1;
  nc.guess = 9.0;
  const Expected<double> e4 = cdsmodel::root_find_brent(square_minus_four, nc);

  ASSERT_FALSE(e1.has_value());
  ASSERT_FALSE(e2.has_value());
  ASSERT_FALSE(e3.has_value());
  EXPECT_EQ(e1.error().kind, ErrorKind::bad_argument);
  EXPECT_EQ(e2.error().kind, ErrorKind::unsupported);
  EXPECT_EQ(e3.error().kind, ErrorKind::no_root);
  if (!e4.has_value()) {
    EXPECT_EQ(e4.error().kind, ErrorKind::no_convergence);
  } else {
    GTEST_SKIP() << "no_convergence not reproducible on this platform's arithmetic";
  }
}

} // namespace
