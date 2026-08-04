#ifndef _SCSYMBOLDATA_H_
#define _SCSYMBOLDATA_H_

typedef uint32_t t_Volume;

#pragma pack(push, 8)


enum TickDirectionEnum
{ DOWN_TICK = -1
, NO_TICK_DIRECTION = 0
, UP_TICK = 1
};

// This structure is used in ACSIL studies.  Therefore, back compatibility
// needs to be maintained.
struct s_SCBasicSymbolData
{
	float DailyOpen_Float = 0; // double
	float DailyHigh_Float = 0; // double
	float DailyLow_Float = 0; // double
	unsigned int Old_DailyVolume = 0;
	int DailyNumberOfTrades = 0;
	float LastTradePrice_Float = 0; // double
	uint32_t LastTradeVolume_Int = 0; // double
	SCDateTime LastTradeDateTime;
	int TickDirection = NO_TICK_DIRECTION;  // TickDirectionEnum
	int LastTradeAtSamePrice = 0;
	char LastTradeAtBidAsk = 0;// can be SC_TS_BID or SC_TS_ASK 
	float Bid_Float = 0; // double
	float Ask_Float = 0; // double
	unsigned int AskQuantity_Int = 0; // double
	unsigned int BidQuantity_Int = 0; // double
	float CurrencyValuePerTick_Float = 0; // double
	float SettlementPrice_Float = 0; // double
	unsigned int OpenInterest = 0;
	unsigned int SharesOutstanding = 0;
	float EarningsPerShare = 0;
	float StrikePrice = 0;
	SCDateTime LastBidAskUpdateDateTime;

	// This must correspond to the pricing format actually used after the
	// price values are adjusted by the DisplayPriceMultiplier.
	float TickSize_Float = 0; // double

	// Standard Sierra Chart price format code.  This must correspond to the
	// pricing format actually used after the price values are adjusted by
	// the DisplayPriceMultiplier.
	int PriceFormat = -1;
	float ContractSize = 0;
	float BuyRolloverInterest = 0;
	float SellRolloverInterest = 0;
	uint8_t TradeIndicator = 0;

	int AccumulatedLastTradeVolume_Int = 0; // double
	SCDateTime LastTradingDateForFutures;
	
	uint8_t TradingStatus = 0;

	// The Date value (SCDateTime) that the current values in the
	// BasicSymbolData object are for.  This is going to be what is
	// considered the trading day date.  So this will be the following day in
	// the case when the trading begins in the prior day at the beginning of
	// the evening session.
	SCDateTime TradingDayDate;

	SCDateTimeMS LastMarketDepthUpdateDateTime;

	float DisplayPriceMultiplier_Float = 1.0f; // double

	SCDateTime SettlementPriceDate;
	SCDateTime DailyOpenPriceDate;
	SCDateTime DailyHighPriceDate;
	SCDateTime DailyLowPriceDate;
	SCDateTime DailyVolumeDate;

	int NumberOfTradesAtCurrentPrice = 0;
	char VolumeValueFormat = 0;// Will become unused
	float LowLimitPrice_Float = 0; // double
	float HighLimitPrice_Float = 0; // double
	uint64_t DailyVolume_Int = 0; // double DailyVolume_Int
	SCDateTime RolloverDate;

	struct s_ImbalanceData
	{
		SCDateTime DateTimeUTC;
		double MatchingQuantity = 0;
		double NonMatchingQuantity = 0;
		double CurrentReferencePrice = 0;
		uint8_t ImbalanceDirection = 0;
		uint8_t CrossType = 0;
	} m_ImbalanceData;

	double DailyBidVolume = 0;
	double DailyAskVolume = 0;

	// Members that are now converted to double will be placed here at the end of the structure. We will implement get and set functions for these members. When setting a member, that has a new double implementation, we also need to set the previous integer or float.

	//New members implemented as doubles
	double DailyOpen = 0;
	double DailyHigh = 0;
	double DailyLow = 0;
	double LastTradePrice = 0;
	double LastTradeVolume = 0;
	double Bid = 0;
	double Ask = 0;
	t_MarketDataQuantity AskQuantity = 0;
	t_MarketDataQuantity BidQuantity = 0;
	double CurrencyValuePerTick = 0;
	double SettlementPrice = 0;
	double TickSize = 0;
	double AccumulatedLastTradeVolume = 0;
	double DisplayPriceMultiplier = 1;
	double LowLimitPrice = 0;
	double HighLimitPrice = 0;
	double DailyVolume = 0;

	//TODO
	//double EarningsPerShare = 0;
	//double StrikePrice = 0;
	//double ContractSize = 0;
	//double BuyRolloverInterest = 0;
	//double SellRolloverInterest = 0;

	/*======================================================================*/
	void Clear()
	{
		*this = {};
	}

	/*======================================================================*/
	void Copy(const s_SCBasicSymbolData& Src)
	{
		if (&Src != this)
			*this = Src;
	}
	
	/*======================================================================*/
	bool operator==(const s_SCBasicSymbolData& Right) const
	{
		return DailyOpen_Float == Right.DailyOpen_Float
			&& DailyHigh_Float == Right.DailyHigh_Float
			&& DailyLow_Float == Right.DailyLow_Float
			&& Old_DailyVolume == Right.Old_DailyVolume
			&& DailyNumberOfTrades == Right.DailyNumberOfTrades
			&& LastTradePrice_Float == Right.LastTradePrice_Float
			&& LastTradeVolume_Int == Right.LastTradeVolume_Int
			&& LastTradeDateTime == Right.LastTradeDateTime
			&& TickDirection == Right.TickDirection
			&& LastTradeAtSamePrice == Right.LastTradeAtSamePrice
			&& LastTradeAtBidAsk == Right.LastTradeAtBidAsk
			&& Bid_Float == Right.Bid_Float
			&& Ask_Float == Right.Ask_Float
			&& AskQuantity_Int == Right.AskQuantity_Int
			&& BidQuantity_Int == Right.BidQuantity_Int
			&& CurrencyValuePerTick_Float == Right.CurrencyValuePerTick_Float
			&& SettlementPrice_Float == Right.SettlementPrice_Float
			&& OpenInterest == Right.OpenInterest
			&& SharesOutstanding == Right.SharesOutstanding
			&& EarningsPerShare == Right.EarningsPerShare
			&& StrikePrice == Right.StrikePrice
			&& LastBidAskUpdateDateTime == Right.LastBidAskUpdateDateTime
			&& TickSize_Float == Right.TickSize_Float
			&& PriceFormat == Right.PriceFormat
			&& ContractSize == Right.ContractSize
			&& BuyRolloverInterest == Right.BuyRolloverInterest
			&& SellRolloverInterest == Right.SellRolloverInterest
			&& TradeIndicator == Right.TradeIndicator
			&& AccumulatedLastTradeVolume_Int == Right.AccumulatedLastTradeVolume_Int
			&& LastTradingDateForFutures == Right.LastTradingDateForFutures
			&& TradingStatus == Right.TradingStatus
			&& TradingDayDate == Right.TradingDayDate
			&& LastMarketDepthUpdateDateTime == Right.LastMarketDepthUpdateDateTime
			&& DisplayPriceMultiplier_Float == Right.DisplayPriceMultiplier_Float
			&& SettlementPriceDate == Right.SettlementPriceDate
			&& DailyOpenPriceDate == Right.DailyOpenPriceDate
			&& DailyHighPriceDate == Right.DailyHighPriceDate
			&& DailyLowPriceDate == Right.DailyLowPriceDate
			&& DailyVolumeDate == Right.DailyVolumeDate
			&& NumberOfTradesAtCurrentPrice == Right.NumberOfTradesAtCurrentPrice
			&& VolumeValueFormat == Right.VolumeValueFormat
			&& LowLimitPrice_Float == Right.LowLimitPrice_Float
			&& HighLimitPrice_Float == Right.HighLimitPrice_Float
			&& DailyVolume_Int == Right.DailyVolume_Int
			&& RolloverDate == Right.RolloverDate
			&& m_ImbalanceData.DateTimeUTC == Right.m_ImbalanceData.DateTimeUTC
			&& m_ImbalanceData.MatchingQuantity == Right.m_ImbalanceData.MatchingQuantity
			&& m_ImbalanceData.NonMatchingQuantity == Right.m_ImbalanceData.NonMatchingQuantity
			&& m_ImbalanceData.CurrentReferencePrice == Right.m_ImbalanceData.CurrentReferencePrice
			&& m_ImbalanceData.ImbalanceDirection == Right.m_ImbalanceData.ImbalanceDirection
			&& m_ImbalanceData.CrossType == Right.m_ImbalanceData.CrossType
			&& DailyBidVolume == Right.DailyBidVolume
			&& DailyAskVolume == Right.DailyAskVolume
			&& DailyOpen == Right.DailyOpen
			&& DailyHigh == Right.DailyHigh
			&& DailyLow == Right.DailyLow
			&& LastTradePrice == Right.LastTradePrice
			&& LastTradeVolume == Right.LastTradeVolume
			&& Bid == Right.Bid
			&& Ask == Right.Ask
			&& AskQuantity == Right.AskQuantity
			&& BidQuantity == Right.BidQuantity
			&& CurrencyValuePerTick == Right.CurrencyValuePerTick
			&& SettlementPrice == Right.SettlementPrice
			&& TickSize == Right.TickSize
			&& AccumulatedLastTradeVolume == Right.AccumulatedLastTradeVolume
			&& DisplayPriceMultiplier == Right.DisplayPriceMultiplier
			&& LowLimitPrice == Right.LowLimitPrice
			&& HighLimitPrice == Right.HighLimitPrice
			&& DailyVolume == Right.DailyVolume;
	}

	/*======================================================================*/
	void MultiplyData(float Multiplier, bool InvertPrices)
	{
		if (Multiplier != 1.0)
		{
			SetAsk(GetAsk() * Multiplier);
			SetBid(GetBid() * Multiplier);
			SetDailyHigh(GetDailyHigh() * Multiplier);
			SetLastTradePrice(GetLastTradePrice() * Multiplier);
			SetDailyLow(GetDailyLow() * Multiplier);
			SetDailyOpen(GetDailyOpen() * Multiplier);
			SetSettlementPrice(GetSettlementPrice() * Multiplier);
		}

		if (InvertPrices)
		{
			SetAsk(SafeInverse(GetAsk()));
			SetBid(SafeInverse(GetBid()));
			SetDailyHigh(SafeInverse(GetDailyHigh()));
			SetLastTradePrice(SafeInverse(GetLastTradePrice()));
			SetDailyLow(SafeInverse(GetDailyLow()));
			SetDailyOpen(SafeInverse(GetDailyOpen()));
			SetSettlementPrice(SafeInverse(GetSettlementPrice()));

		}
	}

	double GetDailyPriceChange() const
	{
		double DailyPriceChange = 0.0;
		if (GetSettlementPrice() != 0.0)
		{
			DailyPriceChange = GetLastTradePrice() - GetSettlementPrice();
		}
		return DailyPriceChange;
	}

	/*======================================================================*/
	double GetDailyOpen() const
	{
		return DailyOpen;
	}
	
	void SetDailyOpen(double Value)
	{
		DailyOpen = Value;
		DailyOpen_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetDailyHigh() const
	{
		return DailyHigh;
	}
	
	void SetDailyHigh(double Value)
	{
		DailyHigh = Value;
		DailyHigh_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetDailyLow() const
	{
		return DailyLow;
	}
	
	void SetDailyLow(double Value)
	{
		DailyLow = Value;
		DailyLow_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetLastTradePrice() const
	{
		return LastTradePrice;
	}
	
	void SetLastTradePrice(double Value)
	{
		LastTradePrice = Value;
		LastTradePrice_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetLastTradeVolume() const
	{
		return LastTradeVolume;
	}
	
	void SetLastTradeVolume(double Value)
	{
		LastTradeVolume = Value;
		LastTradeVolume_Int = static_cast<uint32_t>(Value);
	}

	/*======================================================================*/
	double GetBid() const
	{
		return Bid;
	}
	
	void SetBid(double Value)
	{
		Bid = Value;
		Bid_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetAsk() const
	{
		return Ask;
	}
	
	void SetAsk(double Value)
	{
		Ask = Value;
		Ask_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	t_MarketDataQuantity GetAskQuantity() const
	{
		return AskQuantity;
	}
	
	void SetAskQuantity(t_MarketDataQuantity Value)
	{
		AskQuantity = Value;
		AskQuantity_Int = static_cast <unsigned int>(Value);
	}

	/*======================================================================*/
	t_MarketDataQuantity GetBidQuantity() const
	{
		return BidQuantity;
	}
	
	void SetBidQuantity(t_MarketDataQuantity Value)
	{
		BidQuantity = Value;
		BidQuantity_Int = static_cast<unsigned int>(Value);
	}

	/*======================================================================*/
	double GetCurrencyValuePerTick() const
	{
		return CurrencyValuePerTick;
	}
	
	void SetCurrencyValuePerTick(double Value)
	{
		CurrencyValuePerTick = Value;
		CurrencyValuePerTick_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetSettlementPrice() const
	{
		return SettlementPrice;
	}
	
	void SetSettlementPrice(double Value)
	{
		SettlementPrice = Value;
		SettlementPrice_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetTickSize() const
	{
		return TickSize;
	}
	
	void SetTickSize(double Value)
	{
		TickSize = Value;
		TickSize_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetAccumulatedLastTradeVolume() const
	{
		return AccumulatedLastTradeVolume;
	}
	
	void SetAccumulatedLastTradeVolume(double Value)
	{
		AccumulatedLastTradeVolume = Value;
		AccumulatedLastTradeVolume_Int = static_cast<int>(Value);
	}

	/*======================================================================*/
	double GetDisplayPriceMultiplier() const
	{
		return DisplayPriceMultiplier;
	}
	
	void SetDisplayPriceMultiplier(double Value)
	{
		DisplayPriceMultiplier = Value;
		DisplayPriceMultiplier_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetLowLimitPrice() const
	{
		return LowLimitPrice;
	}
	
	void SetLowLimitPrice(double Value)
	{
		LowLimitPrice = Value;
		LowLimitPrice_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetHighLimitPrice() const
	{
		return HighLimitPrice;
	}
	
	void SetHighLimitPrice(double Value)
	{
		HighLimitPrice = Value;
		HighLimitPrice_Float = static_cast <float>(Value);
	}

	/*======================================================================*/
	double GetDailyVolume() const
	{
		return DailyVolume;
	}
	
	void SetDailyVolume(double Value)
	{
		DailyVolume = Value;
		DailyVolume_Int = static_cast <uint64_t>(Value);
	}

	void AddDailyVolume(double Value)
	{
		DailyVolume += Value;
		DailyVolume_Int += static_cast <uint64_t>(Value);
	}

	/*======================================================================*/
	static float SafeInverse(const float Number)
	{
		if (Number == 0.0f)
			return 0.0f;

		return (1.0f / Number);
	}

	/*======================================================================*/
	static double SafeInverse(const double Number)
	{
		if (Number == 0.0)
			return 0.0;

		return (1.0 / Number);
	}

};

#pragma pack(pop)

#endif
