import pandas as pd

# Load CSV
df = pd.read_csv("gold_futures_data.csv")
df['datetime'] = pd.to_datetime(df['datetime'])
df = df.sort_values('datetime').reset_index(drop=True)

open_trades = []  # Stores currently open trades
closed_trades = []  # Stores closed trades info

volume = 1  # Start with 1 ounce
volume_step = 50  # Increase by 10 ounces on loss

for i in range(5, len(df) - 1):  # Stop at second last candle to have exit candle
    current_datetime = df.loc[i, 'datetime']
    LTP = df.loc[i, 'open']
    closes_prev_1 = df.loc[i - 1, 'close']
    closes_prev_2 = df.loc[i - 2, 'close']

    next_datetime = df.loc[i + 1, 'datetime']
    exit_price = df.loc[i + 1, 'open']  # Exit at open of next candle (can be changed)

    # --- Close all open trades before opening new ---
    # We close all currently open trades at the next candle's open price
    for trade in open_trades:
        pnl_per_ounce = None
        if trade['trade'] == 'buy':
            pnl_per_ounce = exit_price - trade['entry_price']
        else:  # sell
            pnl_per_ounce = trade['entry_price'] - exit_price

        total_pnl = pnl_per_ounce * trade['volume']  # multiply by volume

        duration = next_datetime - trade['entry_datetime']

        trade_record = {
            "entry_datetime": trade['entry_datetime'],
            "exit_datetime": next_datetime,
            "trade_type": trade['trade'],
            "condition": trade['condition'],
            "entry_price": trade['entry_price'],
            "exit_price": exit_price,
            "pnl_per_ounce": pnl_per_ounce,
            "volume": trade['volume'],
            "total_pnl": total_pnl,
            "duration": duration
        }
        closed_trades.append(trade_record)

        # Adjust volume for next trade based on result
        if total_pnl < 0:
            volume = min(volume + volume_step, 100)  # Max 100 ounces
        else:
            volume = 1  # Reset on win

    open_trades = []  # Reset open trades

    # --- Check Condition 1 Buy ---
    if LTP > closes_prev_1 and LTP > closes_prev_2:
        sl = min(closes_prev_1, closes_prev_2)
        open_trades.append({
            "entry_datetime": current_datetime,
            "entry_price": LTP,
            "SL": sl,
            "condition": "Condition 1",
            "trade": "buy",
            "volume": volume
        })
        continue

    # --- Check Condition 1 Sell ---
    if LTP < closes_prev_1 and LTP < closes_prev_2:
        sl = max(closes_prev_1, closes_prev_2)
        open_trades.append({
            "entry_datetime": current_datetime,
            "entry_price": LTP,
            "SL": sl,
            "condition": "Condition 1",
            "trade": "sell",
            "volume": volume
        })
        continue

    # --- Check Condition 2 Buy ---
    found = False
    for j in range(1, 6):
        idx = i - j
        close_j = df.loc[idx, 'close']
        required_price = LTP * 1.005  # 0.5% higher

        if close_j >= required_price:
            lowest_close_5 = df.loc[i - 5:i - 1, 'close'].min()
            open_trades.append({
                "entry_datetime": current_datetime,
                "entry_price": LTP,
                "SL": lowest_close_5,
                "condition": f"Condition 2 (close candle -{j})",
                "trade": "buy",
                "volume": volume
            })
            found = True
            break

    if not found:
        # No trades opened if condition 2 not satisfied
        continue

# Close any remaining open trades at last candle open price
if open_trades:
    last_datetime = df.loc[len(df) - 1, 'datetime']
    last_open = df.loc[len(df) - 1, 'open']
    for trade in open_trades:
        if trade['trade'] == 'buy':
            pnl_per_ounce = last_open - trade['entry_price']
        else:
            pnl_per_ounce = trade['entry_price'] - last_open

        total_pnl = pnl_per_ounce * trade['volume']
        duration = last_datetime - trade['entry_datetime']

        closed_trades.append({
            "entry_datetime": trade['entry_datetime'],
            "exit_datetime": last_datetime,
            "trade_type": trade['trade'],
            "condition": trade['condition'],
            "entry_price": trade['entry_price'],
            "exit_price": last_open,
            "pnl_per_ounce": pnl_per_ounce,
            "volume": trade['volume'],
            "total_pnl": total_pnl,
            "duration": duration
        })

# Print results
print(f"Total trades closed: {len(closed_trades)}\n")

for trade in closed_trades[:20]:  # print first 20 trades
    print(f"Trade Type: {trade['trade_type'].upper()}, Condition: {trade['condition']}, Volume: {trade['volume']} oz")
    print(f"Entry: {trade['entry_datetime']} at {trade['entry_price']:.2f}")
    print(f"Exit:  {trade['exit_datetime']} at {trade['exit_price']:.2f}")
    print(f"P&L per oz: {trade['pnl_per_ounce']:.2f}, Total P&L: {trade['total_pnl']:.2f}, Duration: {trade['duration']}\n")

# Save trades to CSV
trades_df = pd.DataFrame(closed_trades)
trades_df.to_csv('closed_trades_with_volume.csv', index=False)
print("Closed trades saved to 'closed_trades_with_volume.csv'.")
