// tests.cppI

#include "pch.h"
#include "Orderbook.cpp"

namespace googletest = ::testing; // alias the testing namespace

enum class ActionType {
	Add,
	Modify,
	Cancel
};

struct Information {
	ActionType type_;
	OrderType orderType_;
	Side side_;
	Price price_;
	Quantity quantity_;
	OrderId orderId_;
};

using Informations = std::vector<Information>;

struct Result {
	std::size_t allCount_;
	std::size_t bidCount_;
	std::size_t askCount_;

};

struct InputHandler{
private: 
	std::uint64_t ToNumber(const std::string_view& str) cosnt{
		std::int64_t value{};
		std::from_chars(str.data(), str.data() + str.size(), value);
		if (value < 0){
			throw std::logic_error("Value is below 0.");
		}
		return static_cast<std::uint64_t>(value);
	}

	bool TryParseResult(const std::string_view& str, Result& result) const{
		if (str.at(0) != 'R'){ // exit char
			return false;
		}

		auto values = Split(str, ' '); // space delimited
		result.allCount_ = ToNumber(values.at(1));
		result.bidCount_ = ToNumber(values.at(2));
		result.askCount_ = ToNumber(values.at(3));

		return true; // ie "we were able to parse it"
	}

	bool TryParseInformation(const std::string_view& str, Information& info) const{
		auto value = str.at(0);
		auto values = Split(str, ' ');
		if (value == 'A'){ // Add
			info.type_ = ActionType::Add;
			info.side_ = ParseSide(values.at(1));
			info.orderType_ = ParseOrderType(values.at(2));
			info.price_ = ParsePrice(values.at(3));
			info.quantity_ = ParseQuantity(values.at(4));
			info.orderId_ = ParseOrderId(values.at(5));

		} else if (value == 'M') { // Modify
			info.type = ActionType::Modify;
			info.orderId_ = ParseOrderId(values.at(1));
			info.side_ = ParseSide(values.at(2));
			info.price_ = ParsePrice(values.at(3));
			info.quantity_ = ParseQuantity(values.at(4));
		} else if (value == 'C') { // Cancel 
			info.type = ActionType::Cancel;
			info.orderId_ = ParseOrderId(values.at(1));
		} else return false;

		return true;
	}

	std::vector<std::string_view> Split(const std::string_view& str, char delimiter) const{
		std::vector<string_view> columns{};
		std::size_t startIndex{}, endIndex{};
		while ((endIndex = str.find(delimiter, startIndex)) && endIndex != std::string::npos){ // find next place to end at
																							   // continue if end is not found
			auto distance = endIndex - startIndex;
			startIndex = endIndex + 1;
			auto column = str.substr(startIndex, distance);
			columns.push_back(column);
		}
		columns.push_back(str.substr(startIndex));
		return columns;

	}



};




