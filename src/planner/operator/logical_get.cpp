#include "duckdb/planner/operator/logical_get.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/field_writer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_serialization.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/serializer/format_serializer.hpp"
#include "duckdb/common/serializer/format_deserializer.hpp"

namespace duckdb {

LogicalGet::LogicalGet() : LogicalOperator(LogicalOperatorType::LOGICAL_GET) {
}

LogicalGet::LogicalGet(idx_t table_index, TableFunction function, unique_ptr<FunctionData> bind_data,
                       vector<LogicalType> returned_types, vector<string> returned_names)
    : LogicalOperator(LogicalOperatorType::LOGICAL_GET), table_index(table_index), function(std::move(function)),
      bind_data(std::move(bind_data)), returned_types(std::move(returned_types)), names(std::move(returned_names)),
      extra_info() {
}

optional_ptr<TableCatalogEntry> LogicalGet::GetTable() const {
	return TableScanFunction::GetTableEntry(function, bind_data.get());
}

string LogicalGet::ParamsToString() const {
	string result = "";
	for (auto &kv : table_filters.filters) {
		auto &column_index = kv.first;
		auto &filter = kv.second;
		if (column_index < names.size()) {
			result += filter->ToString(names[column_index]);
		}
		result += "\n";
	}
	if (!extra_info.file_filters.empty()) {
		result += "\n[INFOSEPARATOR]\n";
		result += "File Filters: " + extra_info.file_filters;
	}
	if (!function.to_string) {
		return result;
	}
	return result + "\n" + function.to_string(bind_data.get());
}

vector<ColumnBinding> LogicalGet::GetColumnBindings() {
	if (column_ids.empty()) {
		return {ColumnBinding(table_index, 0)};
	}
	vector<ColumnBinding> result;
	if (projection_ids.empty()) {
		for (idx_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
			result.emplace_back(table_index, col_idx);
		}
	} else {
		for (auto proj_id : projection_ids) {
			result.emplace_back(table_index, proj_id);
		}
	}
	if (!projected_input.empty()) {
		if (children.size() != 1) {
			throw InternalException("LogicalGet::project_input can only be set for table-in-out functions");
		}
		auto child_bindings = children[0]->GetColumnBindings();
		for (auto entry : projected_input) {
			D_ASSERT(entry < child_bindings.size());
			result.emplace_back(child_bindings[entry]);
		}
	}
	return result;
}

void LogicalGet::ResolveTypes() {
	if (column_ids.empty()) {
		column_ids.push_back(COLUMN_IDENTIFIER_ROW_ID);
	}

	if (projection_ids.empty()) {
		for (auto &index : column_ids) {
			if (index == COLUMN_IDENTIFIER_ROW_ID) {
				types.emplace_back(LogicalType::ROW_TYPE);
			} else {
				types.push_back(returned_types[index]);
			}
		}
	} else {
		for (auto &proj_index : projection_ids) {
			auto &index = column_ids[proj_index];
			if (index == COLUMN_IDENTIFIER_ROW_ID) {
				types.emplace_back(LogicalType::ROW_TYPE);
			} else {
				types.push_back(returned_types[index]);
			}
		}
	}
	if (!projected_input.empty()) {
		if (children.size() != 1) {
			throw InternalException("LogicalGet::project_input can only be set for table-in-out functions");
		}
		for (auto entry : projected_input) {
			D_ASSERT(entry < children[0]->types.size());
			types.push_back(children[0]->types[entry]);
		}
	}
}

idx_t LogicalGet::EstimateCardinality(ClientContext &context) {
	// join order optimizer does better cardinality estimation.
	if (has_estimated_cardinality) {
		return estimated_cardinality;
	}
	if (function.cardinality) {
		auto node_stats = function.cardinality(context, bind_data.get());
		if (node_stats && node_stats->has_estimated_cardinality) {
			return node_stats->estimated_cardinality;
		}
	}
	return 1;
}

void LogicalGet::Serialize(FieldWriter &writer) const {
	writer.WriteField(table_index);
	writer.WriteRegularSerializableList(returned_types);
	writer.WriteList<string>(names);
	writer.WriteList<column_t>(column_ids);
	writer.WriteList<column_t>(projection_ids);
	writer.WriteSerializable(table_filters);

	FunctionSerializer::SerializeBase<TableFunction>(writer, function, bind_data.get());
	if (!function.serialize) {
		D_ASSERT(!function.deserialize);
		// no serialize method: serialize input values and named_parameters for rebinding purposes
		writer.WriteRegularSerializableList(parameters);
		writer.WriteField<idx_t>(named_parameters.size());
		for (auto &pair : named_parameters) {
			writer.WriteString(pair.first);
			writer.WriteSerializable(pair.second);
		}
		writer.WriteRegularSerializableList(input_table_types);
		writer.WriteList<string>(input_table_names);
	}
	writer.WriteList<column_t>(projected_input);
}

unique_ptr<LogicalOperator> LogicalGet::Deserialize(LogicalDeserializationState &state, FieldReader &reader) {
	auto table_index = reader.ReadRequired<idx_t>();
	auto returned_types = reader.ReadRequiredSerializableList<LogicalType, LogicalType>();
	auto returned_names = reader.ReadRequiredList<string>();
	auto column_ids = reader.ReadRequiredList<column_t>();
	auto projection_ids = reader.ReadRequiredList<column_t>();
	auto table_filters = reader.ReadRequiredSerializable<TableFilterSet>();

	unique_ptr<FunctionData> bind_data;
	bool has_deserialize;
	auto function = FunctionSerializer::DeserializeBaseInternal<TableFunction, TableFunctionCatalogEntry>(
	    reader, state.gstate, CatalogType::TABLE_FUNCTION_ENTRY, bind_data, has_deserialize);

	vector<Value> parameters;
	named_parameter_map_t named_parameters;
	vector<LogicalType> input_table_types;
	vector<string> input_table_names;
	if (!has_deserialize) {
		D_ASSERT(!bind_data);
		parameters = reader.ReadRequiredSerializableList<Value, Value>();

		auto named_parameters_size = reader.ReadRequired<idx_t>();
		for (idx_t i = 0; i < named_parameters_size; i++) {
			auto first = reader.ReadRequired<string>();
			auto second = reader.ReadRequiredSerializable<Value, Value>();
			auto pair = make_pair(first, second);
			named_parameters.insert(pair);
		}

		input_table_types = reader.ReadRequiredSerializableList<LogicalType, LogicalType>();
		input_table_names = reader.ReadRequiredList<string>();
		TableFunctionBindInput input(parameters, named_parameters, input_table_types, input_table_names,
		                             function.function_info.get());

		vector<LogicalType> bind_return_types;
		vector<string> bind_names;
		bind_data = function.bind(state.gstate.context, input, bind_return_types, bind_names);
		if (returned_types != bind_return_types) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different return types than were serialized");
		}
		// names can actually be different because of aliases - only the sizes cannot be different
		if (returned_names.size() != bind_names.size()) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different returned names than were serialized");
		}
	}
	vector<column_t> projected_input;
	reader.ReadList<column_t>(projected_input);

	auto result = make_uniq<LogicalGet>(table_index, function, std::move(bind_data), returned_types, returned_names);
	result->column_ids = std::move(column_ids);
	result->projection_ids = std::move(projection_ids);
	result->table_filters = std::move(*table_filters);
	result->parameters = std::move(parameters);
	result->named_parameters = std::move(named_parameters);
	result->input_table_types = input_table_types;
	result->input_table_names = input_table_names;
	result->projected_input = std::move(projected_input);
	return std::move(result);
}

void LogicalGet::FormatSerialize(FormatSerializer &serializer) const {
	LogicalOperator::FormatSerialize(serializer);
	serializer.WriteProperty("table_index", table_index);
	serializer.WriteProperty("returned_types", returned_types);
	serializer.WriteProperty("names", names);
	serializer.WriteProperty("column_ids", column_ids);
	serializer.WriteProperty("projection_ids", projection_ids);
	serializer.WriteProperty("table_filters", table_filters);
	FunctionSerializer::FormatSerialize(serializer, function, bind_data.get());
	if (!function.format_serialize) {
		D_ASSERT(!function.format_deserialize);
		// no serialize method: serialize input values and named_parameters for rebinding purposes
		serializer.WriteProperty("parameters", parameters);
		serializer.WriteProperty("named_parameters", named_parameters);
		serializer.WriteProperty("input_table_types", input_table_types);
		serializer.WriteProperty("input_table_names", input_table_names);
	}
	serializer.WriteProperty("projected_input", projected_input);
}

unique_ptr<LogicalOperator> LogicalGet::FormatDeserialize(FormatDeserializer &deserializer) {
	auto result = unique_ptr<LogicalGet>(new LogicalGet());
	deserializer.ReadProperty("table_index", result->table_index);
	deserializer.ReadProperty("returned_types", result->returned_types);
	deserializer.ReadProperty("names", result->names);
	deserializer.ReadProperty("column_ids", result->column_ids);
	deserializer.ReadProperty("projection_ids", result->projection_ids);
	deserializer.ReadProperty("table_filters", result->table_filters);
	auto entry = FunctionSerializer::FormatDeserializeBase<TableFunction, TableFunctionCatalogEntry>(
	    deserializer, CatalogType::TABLE_FUNCTION_ENTRY);
	auto &function = entry.first;
	auto has_serialize = entry.second;

	unique_ptr<FunctionData> bind_data;
	if (!has_serialize) {
		deserializer.ReadProperty("parameters", result->parameters);
		deserializer.ReadProperty("named_parameters", result->named_parameters);
		deserializer.ReadProperty("input_table_types", result->input_table_types);
		deserializer.ReadProperty("input_table_names", result->input_table_names);
		TableFunctionBindInput input(result->parameters, result->named_parameters, result->input_table_types,
		                             result->input_table_names, function.function_info.get());

		vector<LogicalType> bind_return_types;
		vector<string> bind_names;
		if (!function.bind) {
			throw InternalException("Table function \"%s\" has neither bind nor (de)serialize", function.name);
		}
		bind_data = function.bind(deserializer.Get<ClientContext &>(), input, bind_return_types, bind_names);
		if (result->returned_types != bind_return_types) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different return types than were serialized");
		}
		// names can actually be different because of aliases - only the sizes cannot be different
		if (result->names.size() != bind_names.size()) {
			throw SerializationException(
			    "Table function deserialization failure - bind returned different returned names than were serialized");
		}
	} else {
		bind_data = FunctionSerializer::FunctionDeserialize(deserializer, function);
	}
	deserializer.ReadProperty("projected_input", result->projected_input);
	return std::move(result);
}

vector<idx_t> LogicalGet::GetTableIndex() const {
	return vector<idx_t> {table_index};
}

string LogicalGet::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return StringUtil::Upper(function.name) + StringUtil::Format(" #%llu", table_index);
	}
#endif
	return StringUtil::Upper(function.name);
}

} // namespace duckdb
