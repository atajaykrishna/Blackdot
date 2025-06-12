# Blackdot
futuresstrategy testings
This Python script performs a backtest of trading strategies on gold futures data. It identifies buy and sell trade signals based on two defined conditions, manages position sizing dynamically based on previous trade results, and outputs a detailed trade log including profit & loss (P&L), duration, and trade volume.

Input Data
The script expects a CSV file (gold_futures_data.csv) containing historical futures price data with the following columns:
--------------------------------------------------------------------------------------
Column	       Data Type	       Description
datetime	   datetime string	  Timestamp of the candle (e.g., 2022-07-19 02:00:00)
open	       float	          Opening price of the candle
close	       float	          Closing price of the candle
-------------------------------------------------------------------------------------
Output
Prints a summary of total closed trades and details for the first 20 trades.

Saves a CSV file named closed_trades_with_volume.csv containing the closed trades data with the following columns:
------------------------------------------------------------
Column	Description
entry_datetime	Entry timestamp of the trade
exit_datetime	Exit timestamp of the trade
trade_type	Trade direction: "buy" or "sell"
condition	Condition under which trade was opened
entry_price	Entry price per ounce
exit_price	Exit price per ounce
pnl_per_ounce	Profit or loss per ounce
volume	Number of ounces traded
total_pnl	Total profit or loss (pnl_per_ounce × volume)
duration	Time duration of the trade (timedelta)
---------------------------------------------------------
Usage
Prepare your CSV data (gold_futures_data.csv) with the structure described above.

Place the CSV in the same directory as the script or update the script with the correct path.

Run the script with Python 3.x:
----------------------------------
Python 3.x

pandas (pip install pandas)
----------------------------------
Notes
The script assumes a fixed time interval between candles.

Modify the volume scaling parameters (volume_step, max volume) in the script as needed.

The exit price is currently set as the open price of the next candle; this can be changed as per strategy needs.
