"""The ISDA CDS Standard Model, ported to C++20."""

from collections.abc import Sequence
import datetime
import enum

from cdsmodel import (
    BadArgument as BadArgument,
    NoConvergence as NoConvergence,
    NoRoot as NoRoot,
    Unsupported as Unsupported
)


__upstream_version__: str = '1.8.3'

class CdsModelError(Exception):
    pass

class TradeTerms:
    """What was traded: the dates and the coupon."""

    def __init__(self, *, start: datetime.date, end: datetime.date, coupon_rate: float = 0.0) -> None:
        """A trade: when it starts, when it ends, and the coupon it pays."""

    @property
    def start(self) -> datetime.date: ...

    @start.setter
    def start(self, arg: datetime.date, /) -> None: ...

    @property
    def end(self) -> datetime.date: ...

    @end.setter
    def end(self, arg: datetime.date, /) -> None: ...

    @property
    def coupon_rate(self) -> float: ...

    @coupon_rate.setter
    def coupon_rate(self, arg: float, /) -> None: ...

    def __repr__(self) -> str: ...

class DayCount(enum.Enum):
    """Day count conventions."""

    ACT_365 = 1

    ACT_365F = 2

    ACT_360 = 3

    THIRTY_360 = 4

    THIRTY_E_360 = 5

    EFFECTIVE_RATE = 8

class ZeroCurve:
    """An interpolated zero-rate curve."""

    @staticmethod
    def from_points(*, base_date: datetime.date, dates: Sequence[datetime.date], rates: Sequence[float], basis: float = 1.0, day_count: DayCount = DayCount.ACT_365F) -> ZeroCurve:
        """Build a curve from a base date and parallel date and rate columns."""

    def discount_factor(self, date: datetime.date) -> float: ...

    def forward_discount_factor(self, start: datetime.date, maturity: datetime.date) -> float: ...

    def zero_rate(self, date: datetime.date) -> float: ...

    @property
    def base_date(self) -> datetime.date: ...

    @property
    def basis(self) -> float: ...

    @property
    def day_count(self) -> DayCount: ...

    def dates(self) -> list[datetime.date]: ...

    def rates(self) -> list[float]: ...

    def __len__(self) -> int: ...

    def __repr__(self) -> str: ...

    @staticmethod
    def build(*, value_date: datetime.date, instruments: Sequence[InstrumentQuote], money_market_day_count: DayCount = DayCount.ACT_360, conventions: SwapConventions = ..., calendar: Calendar = ...) -> ZeroCurve:
        """Bootstrap a zero curve from money-market and swap quotes."""

class Weekend(enum.Flag):
    """Which weekdays are weekends. Combine with `|`."""

    NONE = 0

    MONDAY = 1

    TUESDAY = 2

    WEDNESDAY = 4

    THURSDAY = 8

    FRIDAY = 16

    SATURDAY = 32

    SUNDAY = 64

    STANDARD = 96

class Calendar:
    """Holidays and weekends, for business-day arithmetic."""

    @staticmethod
    def from_dates(*, holidays: Sequence[datetime.date], weekends: Weekend = Weekend.STANDARD) -> Calendar:
        """A calendar from explicit holiday dates."""

    @staticmethod
    def parse(contents: str) -> Calendar:
        """Parse holiday-file contents. Does no IO."""

    @staticmethod
    def none() -> Calendar:
        """Weekends only, no holidays."""

    @staticmethod
    def no_weekends() -> Calendar:
        """Every day is a business day."""

    def is_weekend(self, date: datetime.date) -> bool: ...

    def is_holiday(self, date: datetime.date) -> bool: ...

    def is_business_day(self, date: datetime.date) -> bool: ...

    @property
    def weekends(self) -> Weekend: ...

    @property
    def allows_business_days(self) -> bool: ...

    def holidays(self) -> list[datetime.date]: ...

    def __len__(self) -> int: ...

    def __repr__(self) -> str: ...

class BadDayConv(enum.Enum):
    """What to do when a date lands on a non-business day."""

    FOLLOW = 70

    PREVIOUS = 80

    NONE = 78

    MODIFIED = 77

class PeriodUnit(enum.Enum):
    """The unit a DateInterval counts in."""

    DAYS = 0

    MONTHS = 1

class StubPosition(enum.Enum):
    """Which end carries the odd period."""

    FRONT = 0

    BACK = 1

class StubLength(enum.Enum):
    """Whether the odd period absorbs its neighbour."""

    SHORT = 0

    LONG = 1

class DateInterval:
    """A count of days or months."""

    def __init__(self, *, periods: int, unit: PeriodUnit = PeriodUnit.MONTHS) -> None: ...

    @property
    def periods(self) -> int: ...

    @periods.setter
    def periods(self, arg: int, /) -> None: ...

    @property
    def unit(self) -> PeriodUnit: ...

    @unit.setter
    def unit(self, arg: PeriodUnit, /) -> None: ...

    def __eq__(self, arg: DateInterval, /) -> bool: ...

    def __repr__(self) -> str: ...

class StubMethod:
    """Where the odd period sits, and how long it is."""

    def __init__(self, *, position: StubPosition = StubPosition.FRONT, length: StubLength = StubLength.SHORT) -> None: ...

    @property
    def position(self) -> StubPosition: ...

    @position.setter
    def position(self, arg: StubPosition, /) -> None: ...

    @property
    def length(self) -> StubLength: ...

    @length.setter
    def length(self, arg: StubLength, /) -> None: ...

    def __eq__(self, arg: StubMethod, /) -> bool: ...

    def __repr__(self) -> str: ...

class Valuation:
    """When the valuation happens."""

    def __init__(self, *, today: datetime.date, cash_settle: datetime.date, step_in: datetime.date) -> None: ...

    @property
    def today(self) -> datetime.date: ...

    @today.setter
    def today(self, arg: datetime.date, /) -> None: ...

    @property
    def cash_settle(self) -> datetime.date: ...

    @cash_settle.setter
    def cash_settle(self, arg: datetime.date, /) -> None: ...

    @property
    def step_in(self) -> datetime.date: ...

    @step_in.setter
    def step_in(self, arg: datetime.date, /) -> None: ...

    def __repr__(self) -> str: ...

class Conventions:
    """What a desk sets once."""

    def __init__(self, *, pay_accrual_on_default: bool = True, coupon_interval: DateInterval = ..., stub: StubMethod = ..., day_count: DayCount = DayCount.ACT_360, bad_day: BadDayConv = BadDayConv.NONE, holidays: Sequence[Calendar] = ...) -> None: ...

    @property
    def pay_accrual_on_default(self) -> bool: ...

    @pay_accrual_on_default.setter
    def pay_accrual_on_default(self, arg: bool, /) -> None: ...

    @property
    def coupon_interval(self) -> DateInterval: ...

    @coupon_interval.setter
    def coupon_interval(self, arg: DateInterval, /) -> None: ...

    @property
    def stub(self) -> StubMethod: ...

    @stub.setter
    def stub(self, arg: StubMethod, /) -> None: ...

    @property
    def day_count(self) -> DayCount: ...

    @day_count.setter
    def day_count(self, arg: DayCount, /) -> None: ...

    @property
    def bad_day(self) -> BadDayConv: ...

    @bad_day.setter
    def bad_day(self, arg: BadDayConv, /) -> None: ...

    @property
    def holidays(self) -> list[Calendar]: ...

    @holidays.setter
    def holidays(self, arg: Sequence[Calendar], /) -> None: ...

    def __repr__(self) -> str: ...

class MarketData:
    """What the market says today."""

    def __init__(self, *, discount: ZeroCurve, spread: ZeroCurve, recovery: float) -> None: ...

    @property
    def discount(self) -> ZeroCurve: ...

    @discount.setter
    def discount(self, arg: ZeroCurve, /) -> None: ...

    @property
    def spread(self) -> ZeroCurve: ...

    @spread.setter
    def spread(self, arg: ZeroCurve, /) -> None: ...

    @property
    def recovery(self) -> float: ...

    @recovery.setter
    def recovery(self, arg: float, /) -> None: ...

    def __repr__(self) -> str: ...

class FlatMarket:
    """A discount curve and a flat quote."""

    def __init__(self, *, discount: ZeroCurve, quote: float, recovery: float) -> None: ...

    @property
    def discount(self) -> ZeroCurve: ...

    @discount.setter
    def discount(self, arg: ZeroCurve, /) -> None: ...

    @property
    def quote(self) -> float: ...

    @quote.setter
    def quote(self, arg: float, /) -> None: ...

    @property
    def recovery(self) -> float: ...

    @recovery.setter
    def recovery(self, arg: float, /) -> None: ...

    def __repr__(self) -> str: ...

class InstrumentKind(enum.Enum):
    """What a curve benchmark is."""

    MONEY_MARKET = 0

    SWAP = 1

class InterpType(enum.Enum):
    """How the curve interpolates between points."""

    LINEAR = 0

    FLAT_FORWARDS = 1

class StubPosDefault(enum.Enum):
    """Where a swap's odd period goes."""

    AUTO = 0

    FRONT = 1

    BACK = 2

class InstrumentQuote:
    """One benchmark: what it is, when it matures, its quote."""

    def __init__(self, *, kind: InstrumentKind, date: datetime.date, rate: float) -> None: ...

    @property
    def kind(self) -> InstrumentKind: ...

    @kind.setter
    def kind(self, arg: InstrumentKind, /) -> None: ...

    @property
    def date(self) -> datetime.date: ...

    @date.setter
    def date(self, arg: datetime.date, /) -> None: ...

    @property
    def rate(self) -> float: ...

    @rate.setter
    def rate(self, arg: float, /) -> None: ...

    def __eq__(self, arg: InstrumentQuote, /) -> bool: ...

    def __repr__(self) -> str: ...

class SwapConventions:
    """How the swap leg of a curve is quoted."""

    def __init__(self, *, fixed_frequency: int = 2, float_frequency: int = 2, fixed_day_count: DayCount = DayCount.ACT_360, float_day_count: DayCount = DayCount.ACT_360, interp: InterpType = InterpType.FLAT_FORWARDS, bad_day: BadDayConv = BadDayConv.NONE, stub_position: StubPosDefault = StubPosDefault.AUTO) -> None: ...

    @property
    def fixed_frequency(self) -> int: ...

    @fixed_frequency.setter
    def fixed_frequency(self, arg: int, /) -> None: ...

    @property
    def float_frequency(self) -> int: ...

    @float_frequency.setter
    def float_frequency(self, arg: int, /) -> None: ...

    @property
    def fixed_day_count(self) -> DayCount: ...

    @fixed_day_count.setter
    def fixed_day_count(self, arg: DayCount, /) -> None: ...

    @property
    def float_day_count(self) -> DayCount: ...

    @float_day_count.setter
    def float_day_count(self, arg: DayCount, /) -> None: ...

    @property
    def interp(self) -> InterpType: ...

    @interp.setter
    def interp(self, arg: InterpType, /) -> None: ...

    @property
    def bad_day(self) -> BadDayConv: ...

    @bad_day.setter
    def bad_day(self, arg: BadDayConv, /) -> None: ...

    @property
    def stub_position(self) -> StubPosDefault: ...

    @stub_position.setter
    def stub_position(self, arg: StubPosDefault, /) -> None: ...

    def __repr__(self) -> str: ...

class CashFlow:
    """One payment: when, and how much."""

    @property
    def date(self) -> datetime.date: ...

    @property
    def amount(self) -> float: ...

    def __repr__(self) -> str: ...

class DefaultAccrual:
    """Accrued interest owed on a defaulted CDS."""

    @property
    def days(self) -> int: ...

    @property
    def amount(self) -> float: ...

    def __repr__(self) -> str: ...

def price(trade: TradeTerms, valuation: Valuation, conventions: Conventions, market: MarketData, *, clean: bool = True) -> float:
    """The present value of a CDS."""

def par_spread(trade: TradeTerms, valuation: Valuation, conventions: Conventions, market: MarketData) -> float:
    """The coupon at which the CDS would price to zero."""

def fee_leg_flows(trade: TradeTerms, conventions: Conventions, *, notional: float = 1.0) -> list[CashFlow]:
    """The fee leg's cash flows, one per coupon."""

def default_accrual(trade: TradeTerms, conventions: Conventions, *, trade_date: datetime.date, event_determination_date: datetime.date, notional: float = 1.0) -> DefaultAccrual:
    """Accrued interest owed on a CDS that has defaulted."""
