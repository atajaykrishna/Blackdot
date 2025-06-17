import pandas as pd

# Load data
df = pd.read_csv("gold_futures_data.csv")
df['datetime'] = pd.to_datetime(df['datetime'])
df = df.sort_values('datetime').reset_index(drop=True)

open_trade = None
closed_trades = []

for i in range(7, len(df) - 1):  # Start after 5 candles
    current_row = df.loc[i]
    open_today = current_row['open']  # Current candle open
    close_today = current_row['close']  # Current candle close
    high_today = current_row['high']  # Current candle high
    low_today = current_row['low']  # Current candle low
    close1 = df.loc[i - 1, 'close']  # Close of the first previous candle
    close2 = df.loc[i - 2, 'close']  # Close of the second previous candle
    next_open = df.loc[i + 1, 'open']
    next_time = df.loc[i + 1, 'datetime']
    current_time = current_row['datetime']

    # --- Initialize trade_signal to None ---
    trade_signal = None  # Ensure trade_signal is always defined

    # --- Condition 1 --- (High and Low Price Comparison for Buy/Sell)
    if high_today > close1 and low_today > close2:
        # --- Condition 2 (Buy Condition) ---
        buy_cond2 = False
        sl_candles = []

        # Step 1: Check the first two candles' price strength condition
        if close2 > close1 * 1.005:
            if high_today > close2:  # Compare the high of today with close2
                buy_cond2 = True
                sl_candles = [close1, close2]
        else:
            # Step 2: Check the next 3 to 5 candles
            for j in range(3, 6):  # Check for the next 3 to 5 candles
                compare_close = df.loc[i - j, 'close']
                if compare_close > close1 * 1.005:  # If the close is greater than 0.5% of close1
                    if high_today > compare_close:  # If high_today is greater than this candle's close
                        buy_cond2 = True
                        sl_candles.append(compare_close)

        # If buy_cond2 is True, set SL to the minimum of the sl_candles
        if buy_cond2:
            sl = min(sl_candles)
            trade_signal = 'buy'

    elif high_today < close1 and low_today < close2:
        # --- Condition 2 (Sell Condition) ---
        sell_cond2 = False
        sl_candles = []

        # Check if close2 > close1 * 1.005
        if close2 > close1 * 1.005:
            # If close2 > close1 * 1.005, check if high_today < close2
            if low_today < close2:
                sell_cond2 = True
                sl_candles = [close1, close2]  # SL is the highest of close1 and close2
        else:
            # If close2 is not greater than close1 * 1.005, check the next 3 to 5 candles
            for j in range(3, 6):  # Check for the next 3 to 5 candles
                compare_close = df.loc[i - j, 'close']
                if compare_close > close1 * 1.005:  # If the close is greater than 0.5% of close1
                    if low_today < compare_close:  # If low_today is lower than this candle's close
                        sell_cond2 = True
                        sl_candles.append(compare_close)

        if sell_cond2:
            sl = max(sl_candles)  # Set SL as the highest close price among the candles
            trade_signal = 'sell'

    # --- Trade Decision ---
    if trade_signal:
        # Close opposite trade
        if open_trade and open_trade['trade'] != trade_signal:
            trade = open_trade
            exit_time = current_time  # Exit the current trade on the same day as the new entry
            pnl = next_open - trade['entry_price'] if trade['trade'] == 'buy' else trade['entry_price'] - next_open
            duration = exit_time - trade['entry_datetime']
            closed_trades.append({
                "entry_datetime": trade['entry_datetime'],
                "exit_datetime": exit_time,
                "trade_type": trade['trade'],
                "condition": trade['condition'],
                "entry_price": trade['entry_price'],
                "exit_price": next_open,
                "pnl": pnl,
                "duration": duration
            })
            open_trade = None

        # Open new trade
        if not open_trade:
            open_trade = {
                "entry_datetime": current_time,
                "entry_price": open_today,
                "SL": sl,
                "condition": "Both conditions met",
                "trade": trade_signal
            }

# Final Trade Closure
if open_trade:
    final_open = df.loc[len(df) - 1, 'open']
    final_time = df.loc[len(df) - 1, 'datetime']
    trade = open_trade
    pnl = final_open - trade['entry_price'] if trade['trade'] == 'buy' else trade['entry_price'] - final_open
    duration = final_time - trade['entry_datetime']
    closed_trades.append({
        "entry_datetime": trade['entry_datetime'],
        "exit_datetime": final_time,
        "trade_type": trade['trade'],
        "condition": trade['condition'],
        "entry_price": trade['entry_price'],
        "exit_price": final_open,
        "pnl": pnl,
        "duration": duration
    })


# Save results to CSV
trades_df = pd.DataFrame(closed_trades)
trades_df.to_csv('closed_trades.csv', index=False)

# Print trade summary
print(f"Total trades closed: {len(closed_trades)}")
print(trades_df.head(10))
