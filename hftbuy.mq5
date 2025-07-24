#include <Trade\Trade.mqh>
CTrade trade;

input string   InpSymbol     = "BRILLANTG.fc2sf";
input double   EntryPrice    = 5.0;
input double   ExitPrice     = 7.0;
input double   Volume        = 1.0;
input int      NumTrades     = 1;

bool entered = false;
int trades_done = 0;

int OnInit()
{
   entered = false;
   trades_done = 0;
   return(INIT_SUCCEEDED);
}

void OnTick()
{
   if (trades_done >= NumTrades)
      return;

   double bid = SymbolInfoDouble(InpSymbol, SYMBOL_BID);
   double ask = SymbolInfoDouble(InpSymbol, SYMBOL_ASK);

   // Enter when price is low
   if (!entered && bid <= EntryPrice)
   {
      if (trade.Buy(Volume, InpSymbol, ask, 0, 0, "1"))
      {
         entered = true;
         Print("Entered Buy at ", ask);
      }
   }
   // Exit when price is high
   else if (entered && bid >= ExitPrice)
   {
      for (int i = PositionsTotal() - 1; i >= 0; i--)
      {
         if (PositionGetTicket(i) != 0 && PositionGetSymbol(i) == InpSymbol)
         {
            if (PositionGetString(POSITION_COMMENT) == "1")
            {
               ulong ticket = PositionGetInteger(POSITION_TICKET);
               if (trade.PositionClose(ticket))
               {
                  Print("Exited position with comment '1' at ", bid);
                  trades_done++;
                  entered = false;
               }
            }
         }
      }
   }
}
