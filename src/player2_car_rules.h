#pragma once
#include <string>

namespace fm3 {
// Use the same strict event predicate for both configurations. Only models
// owned and installed in the shared garage can contribute a stock option.
struct Player2CarRules {
  std::string view,predicate;
  std::string StockPredicate() const {
    auto result=predicate;
    for(size_t at=0;(at=result.find("Garage.",at))!=std::string::npos;at+=6)
      result.replace(at,7,"Stock.");
    return result;
  }
  std::string From() const {
    return " FROM "+view+" AS Garage INNER JOIN Data_Car AS Stock ON Stock.Id=Garage.CarId";
  }
  std::string Available() const {
    return "Garage.IsInstalled=1 AND Garage.IsPurchased=1 AND Stock.IsInstalled=1 AND Stock.IsPurchased=1 AND (("+predicate+") OR ("+StockPredicate()+"))";
  }
  std::string UseStock() const {return "CASE WHEN ("+predicate+") THEN 0 ELSE 1 END";}
  std::string Effective(const std::string& column) const {
    return "CASE WHEN ("+predicate+") THEN Garage."+column+" ELSE Stock."+column+" END";
  }
  std::string Select(const std::string& columns,const std::string& extra="") const {
    return "SELECT "+columns+From()+" WHERE "+Available()+extra;
  }
  std::string NativeQuery() const {
    // The stock game's car adapter already uses IsQualified=0 to display
    // the stock configuration of an owned car with unsuitable upgrades.
    return Select("CASE WHEN ("+predicate+") THEN 1 ELSE 0 END AS IsQualified, Garage.*, Garage.Id AS GarageId, 1 AS GarageStatus")+
      " ORDER BY "+Effective("ClassID")+", "+Effective("PerformanceIndex")+", Garage.Id";
  }
};
}
