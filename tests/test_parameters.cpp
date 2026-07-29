#include "gtest/gtest.h"
#include "rpp_cpp/plugin.hpp"
#include "rpp_cpp/data_model.hpp"
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/parameter_handler.hpp"


RPP_PARAM_STRUCT(TestStruct1,
    RPP_MEMBER(int, width, 640),
    RPP_MEMBER(std::string, height, "480"),
    RPP_MEMBER(double, fps, 30.0)
)

using map_type = std::map<std::string, TestStruct1>;
RPP_PARAM_STRUCT(TestStruct2,
    RPP_MEMBER(TestStruct1, test_struct, TestStruct1{42, "42", 42.0}),
    RPP_MEMBER(std::vector<TestStruct1>, test_struct_list, std::vector<TestStruct1>{
        TestStruct1{640, "480", 30.0},
        TestStruct1{800, "600", 60.0}
    }),
    RPP_MEMBER(map_type, test_struct_map, map_type{
        {"first", TestStruct1{640, "480", 30.0}},
        {"second", TestStruct1{800, "600", 60.0}}
    })
)

using map_type2 = std::map<std::string, std::string>;
RPP_PARAM_STRUCT(TestStruct3,
    RPP_MEMBER(int, width, 1),
    RPP_MEMBER(std::string, height, "2"),
    RPP_MEMBER(float, fps, 3.0),
    RPP_MEMBER(map_type2, dict_var , map_type2{}),
    RPP_MEMBER(TestStruct1, test_struct, TestStruct1{})
)

class TestParameters : public ::testing::Test {

public:

    using ParametersDescription = std::vector<rpp::params::ParameterDescription>;
    using ParameterDescription = rpp::params::ParameterDescription;
    using ParameterValue = rpp::params::ParameterValue;


    static std::string test_lib;
    static std::string test_data_dir;
    static bool initialization_successful;
    static std::unique_ptr<pybind11::scoped_interpreter> python_interpreter;

protected:
    static void SetUpTestSuite() {
        test_lib = "test_lib";
        test_data_dir = std::getenv("TEST_DATA_DIR");
        python_interpreter =
            std::make_unique<pybind11::scoped_interpreter>();
        initialization_successful = true;
    }

    static void TearDownTestSuite() {
        // std::filesystem::remove_all(tmp_dir);
    }

};

std::string TestParameters::test_lib = "";
std::string TestParameters::test_data_dir = "";
std::unique_ptr<pybind11::scoped_interpreter> TestParameters::python_interpreter = nullptr;
bool TestParameters::initialization_successful = false;

TEST_F(TestParameters, TestParameterStructReflection) {
    TestStruct1 struct1;
    ASSERT_EQ(struct1.width, 640);
    ASSERT_EQ(struct1.height, "480");
    ASSERT_EQ(struct1.fps, 30.0);

    TestStruct2 struct2;
    ASSERT_EQ(struct2.test_struct.width, 42);
    ASSERT_EQ(struct2.test_struct.height, "42");
    ASSERT_EQ(struct2.test_struct.fps, 42.0);
    ASSERT_EQ(struct2.test_struct_list.size(), 2);
    ASSERT_EQ(struct2.test_struct_list[0].width, 640);
    ASSERT_EQ(struct2.test_struct_list[0].height, "480");
    ASSERT_EQ(struct2.test_struct_list[0].fps, 30.0);
    ASSERT_EQ(struct2.test_struct_list[1].width, 800);
    ASSERT_EQ(struct2.test_struct_list[1].height, "600");
    ASSERT_EQ(struct2.test_struct_list[1].fps, 60.0);

    ASSERT_EQ(struct2.test_struct_map.size(), 2);
    ASSERT_EQ(struct2.test_struct_map["first"].width, 640);
    ASSERT_EQ(struct2.test_struct_map["first"].height, "480");
    ASSERT_EQ(struct2.test_struct_map["first"].fps, 30.0);
    ASSERT_EQ(struct2.test_struct_map["second"].width, 800);
    ASSERT_EQ(struct2.test_struct_map["second"].height, "600");
    ASSERT_EQ(struct2.test_struct_map["second"].fps, 60.0);
}

TEST_F(TestParameters, TestCreateParametersDescriptionAndConversion) {
    ParametersDescription parameters_description
        {
            ParameterDescription::create(
                "test_param_double", double{30.0}),
            ParameterDescription::create(
                "test_param_int", int{40}),
            ParameterDescription::create(
                "test_param_string", std::string{"Test"}),
            ParameterDescription::create(
                "test_param_bool", bool{true}),
            ParameterDescription::create(
                "test_param_struct",
                TestStruct1{
                    1, "2", 3.0
                }),
            ParameterDescription::create(
                "test_param_list_primitive", std::vector<int>{1, 2, 3}),
            ParameterDescription::create(
                "test_param_list_struct", std::vector<TestStruct1>{
                    TestStruct1{1, "2", 3.0},
                    TestStruct1{4, "5", 6.0}
                }),
            ParameterDescription::create(
                "test_param_map_struct", std::map<std::string, TestStruct1>{
                    {"first", TestStruct1{1, "2", 3.0}},
                    {"second", TestStruct1{4, "5", 6.0}}
                }),
            ParameterDescription::create(
                "test_param_struct2", TestStruct2{})
        };

    std::map<std::string, ParameterValue> parameters_instance
    {
        {"test_param_double", 31.0},
        {"test_param_int", 41},
        // {"test_param_string", std::string{"Test1"}}, // test default value for string
        {"test_param_bool", true},
        {"test_param_struct_invalid", std::map<std::string, ParameterValue>{
            // {"width", 2}, to test default value for width
            {"height", std::string{"3"}},
            {"fps", 4.0}
        }},
        {"test_param_struct", std::map<std::string, ParameterValue>{
            {"width", 2},
            {"height", std::string{"3"}},
            {"fps", 4.0}
        }},
        {"test_param_list_primitive", std::vector<ParameterValue>{2, 3, 4}},
        {"test_param_list_struct", std::vector<ParameterValue>{
            ParameterValue(std::map<std::string, ParameterValue>{
                {"width", 2},
                {"height", std::string{"3"}},
                {"fps", 4.0}
            }),
            ParameterValue(std::map<std::string, ParameterValue>{
                {"width", 5},
                {"height", std::string{"6"}},
                {"fps", 7.0}
            })
        }},
        {"test_param_map_struct", std::map<std::string, ParameterValue>{
            {"first", ParameterValue(std::map<std::string, ParameterValue>{
                {"width", 2},
                {"height", std::string{"3"}},
                {"fps", 4.0}
            })},
            {"second", ParameterValue(std::map<std::string, ParameterValue>{
                {"width", 5},
                {"height", std::string{"6"}},
                {"fps", 7.0}
            })}
        }},
        {"test_param_struct2", ParameterValue(std::map<std::string, ParameterValue>{
            {"test_struct", ParameterValue(std::map<std::string, ParameterValue>{
                {"width", 3},
                {"height", std::string{"4"}},
                {"fps", 5.0}
            })},
            {"test_struct_list", ParameterValue(std::vector<ParameterValue>{
                ParameterValue(std::map<std::string, ParameterValue>{
                    {"width", 3},
                    {"height", std::string{"4"}},
                    {"fps", 5.0}
                }),
                ParameterValue(std::map<std::string, ParameterValue>{
                    {"width", 6},
                    {"height", std::string{"7"}},
                    {"fps", 8.0}
                })
            })},
            {"test_struct_map", ParameterValue(std::map<std::string, ParameterValue>{
                {"first", ParameterValue(std::map<std::string, ParameterValue>{
                    {"width", 3},
                    {"height", std::string{"4"}},
                    {"fps", 5.0}
                })},
                {"second", ParameterValue(std::map<std::string, ParameterValue>{
                    {"width", 6},
                    {"height", std::string{"7"}},
                    {"fps", 8.0}
                })}
            })}
        })}
    };

    std::unique_ptr<rpp::params::Parameters> params;
    rpp::params::ParameterHandler::resolve_params(parameters_description, parameters_instance, params);

    ASSERT_EQ(params->get<double>("test_param_double"), 31.0);
    ASSERT_EQ(params->get<int>("test_param_int"), 41);
    ASSERT_EQ(params->get<std::string>("test_param_string"), "Test"); // test default value for string
    ASSERT_EQ(params->get<bool>("test_param_bool"), true);
    // this throws an exception if the parameter is not found or if the type does not match
    try {
        auto struct1 = params->get<TestStruct1>("test_param_struct_invalid");
    } catch (const std::exception& e) {
        auto message = std::string(e.what());
        ASSERT_TRUE(message.find("Field 'width' not found") == std::string::npos);
    }

    ASSERT_EQ(params->get<TestStruct1>("test_param_struct").width, 2);
    ASSERT_EQ(params->get<TestStruct1>("test_param_struct").height, "3");
    ASSERT_EQ(params->get<TestStruct1>("test_param_struct").fps, 4.0);

    auto expected = std::vector<int>{2, 3, 4};
    ASSERT_EQ(params->get<std::vector<int>>("test_param_list_primitive"), expected);
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct").size(), 2);
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[0].width, 2);
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[0].height, "3");
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[0].fps, 4.0);
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[1].width, 5);
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[1].height, "6");
    ASSERT_EQ(params->get<std::vector<TestStruct1>>("test_param_list_struct")[1].fps, 7.0);


    auto test_map_struct = params->get<std::map<std::string, TestStruct1>>("test_param_map_struct");
    ASSERT_EQ(test_map_struct.size(), 2);
    ASSERT_EQ(test_map_struct["first"].width, 2);
    ASSERT_EQ(test_map_struct["first"].height, "3");
    ASSERT_EQ(test_map_struct["first"].fps, 4.0);
    ASSERT_EQ(test_map_struct["second"].width, 5);
    ASSERT_EQ(test_map_struct["second"].height, "6");
    ASSERT_EQ(test_map_struct["second"].fps, 7.0);

    auto test_struct2 = params->get<TestStruct2>("test_param_struct2");
    ASSERT_EQ(test_struct2.test_struct.width, 3);
    ASSERT_EQ(test_struct2.test_struct.height, "4");
    ASSERT_EQ(test_struct2.test_struct.fps, 5.0);
    ASSERT_EQ(test_struct2.test_struct_list.size(), 2);
    ASSERT_EQ(test_struct2.test_struct_list[0].width, 3);
    ASSERT_EQ(test_struct2.test_struct_list[0].height, "4");
    ASSERT_EQ(test_struct2.test_struct_list[0].fps, 5.0);
    ASSERT_EQ(test_struct2.test_struct_list[1].width, 6);
    ASSERT_EQ(test_struct2.test_struct_list[1].height, "7");
    ASSERT_EQ(test_struct2.test_struct_list[1].fps, 8.0);
    ASSERT_EQ(test_struct2.test_struct_map.size(), 2);
    ASSERT_EQ(test_struct2.test_struct_map["first"].width, 3);
    ASSERT_EQ(test_struct2.test_struct_map["first"].height, "4");
    ASSERT_EQ(test_struct2.test_struct_map["first"].fps, 5.0);
    ASSERT_EQ(test_struct2.test_struct_map["second"].width, 6);
    ASSERT_EQ(test_struct2.test_struct_map["second"].height, "7");
    ASSERT_EQ(test_struct2.test_struct_map["second"].fps, 8.0);
}

TEST_F(TestParameters, TestLoadParametersFromPythonModule) {
    auto parameter_handler = std::make_unique<rpp::params::ParameterHandler>(
        test_data_dir + "/test_component", *python_interpreter);

    auto loaded_params = parameter_handler->load_parameters_from_python_module();

    ASSERT_EQ(loaded_params.size(), 10);
    ASSERT_EQ(loaded_params["float_var"].get<float>(), 5.0f);
    ASSERT_EQ(loaded_params["int_var"].get<int>(), 1);
    ASSERT_EQ(loaded_params["str_var"].get<std::string>(), "test");
    auto list = loaded_params["list_var"].get<std::vector<ParameterValue>>();
    ASSERT_EQ(list.size(), 3);
    ASSERT_EQ(list[0].get<int>(), 1);
    ASSERT_EQ(list[1].get<int>(), 2);
    ASSERT_EQ(list[2].get<int>(), 3);
    auto dict = loaded_params["dict_var"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(dict.size(), 1);
    ASSERT_EQ(dict["key"].get<std::string>(), "value");
    auto list_dict = loaded_params["list_dict_var"].get<std::vector<ParameterValue>>();
    ASSERT_EQ(list_dict.size(), 2);
    auto dict1 = list_dict[0].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(dict1.size(), 1);
    ASSERT_EQ(dict1["key1"].get<std::string>(), "value1");
    auto dict2 = list_dict[1].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(dict2.size(), 1);
    ASSERT_EQ(dict2["key2"].get<std::string>(), "value2");
    auto dict_list = loaded_params["dict_list_var"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(dict_list.size(), 2);
    ASSERT_EQ(dict_list["key1"].get<std::vector<ParameterValue>>().size(), 3);
    ASSERT_EQ(dict_list["key1"].get<std::vector<ParameterValue>>()[0].get<int>(), 1);
    ASSERT_EQ(dict_list["key1"].get<std::vector<ParameterValue>>()[1].get<int>(), 2);
    ASSERT_EQ(dict_list["key1"].get<std::vector<ParameterValue>>()[2].get<int>(), 3);
    ASSERT_EQ(dict_list["key2"].get<std::vector<ParameterValue>>().size(), 3);
    ASSERT_EQ(dict_list["key2"].get<std::vector<ParameterValue>>()[0].get<int>(), 4);
    ASSERT_EQ(dict_list["key2"].get<std::vector<ParameterValue>>()[1].get<int>(), 5);
    ASSERT_EQ(dict_list["key2"].get<std::vector<ParameterValue>>()[2].get<int>(), 6);
    auto struct_var = loaded_params["struct_var"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var["width"].get<int>(), 1);
    ASSERT_EQ(struct_var["height"].get<std::string>(), "2");
    ASSERT_EQ(struct_var["fps"].get<float>(), 3.0);
    auto struct_var2 = loaded_params["complext_struct_var"].get<std::map<std::string, ParameterValue>>();
    auto struct_var2_test_struct = struct_var2["test_struct"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct["width"].get<int>(), 1);
    ASSERT_EQ(struct_var2_test_struct["height"].get<std::string>(), "2");
    ASSERT_EQ(struct_var2_test_struct["fps"].get<float>(), 3.0);
    auto struct_var2_test_struct_list = struct_var2["test_struct_list"].get<std::vector<ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_list.size(), 2);
    auto struct_var2_test_struct_list_0 = struct_var2_test_struct_list[0].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_list_0["width"].get<int>(), 1);
    ASSERT_EQ(struct_var2_test_struct_list_0["height"].get<std::string>(), "2");
    ASSERT_EQ(struct_var2_test_struct_list_0["fps"].get<float>(), 3.0);
    auto struct_var2_test_struct_list_1 = struct_var2_test_struct_list[1].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_list_1["width"].get<int>(), 2);
    ASSERT_EQ(struct_var2_test_struct_list_1["height"].get<std::string>(), "3");
    ASSERT_EQ(struct_var2_test_struct_list_1["fps"].get<float>(), 4.0);
    auto struct_var2_test_struct_map = struct_var2["test_struct_map"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_map.size(), 2);
    auto struct_var2_test_struct_map_first = struct_var2_test_struct_map["first"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_map_first["width"].get<int>(), 1);
    ASSERT_EQ(struct_var2_test_struct_map_first["height"].get<std::string>(), "2");
    ASSERT_EQ(struct_var2_test_struct_map_first["fps"].get<float>(), 3.0);
    auto struct_var2_test_struct_map_second = struct_var2_test_struct_map["second"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(struct_var2_test_struct_map_second["width"].get<int>(), 5);
    ASSERT_EQ(struct_var2_test_struct_map_second["height"].get<std::string>(), "6");
    ASSERT_EQ(struct_var2_test_struct_map_second["fps"].get<float>(), 7.0);
    auto non_dataclass_var = loaded_params["non_dataclass_var"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(non_dataclass_var["width"].get<int>(), 1);
    ASSERT_EQ(non_dataclass_var["height"].get<std::string>(), "2");
    ASSERT_EQ(non_dataclass_var["fps"].get<float>(), 3.0);
    auto non_dataclass_dict_var = non_dataclass_var["dict_var"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(non_dataclass_dict_var["key"].get<std::string>(), "value");
    auto non_dataclass_test_struct = non_dataclass_var["test_struct"].get<std::map<std::string, ParameterValue>>();
    ASSERT_EQ(non_dataclass_test_struct["width"].get<int>(), 1);
    ASSERT_EQ(non_dataclass_test_struct["height"].get<std::string>(), "2");
    ASSERT_EQ(non_dataclass_test_struct["fps"].get<float>(), 3.0);
}

TEST_F(TestParameters, TestLoadParametersFromPythonModuleAndResolve) {

    auto parameter_handler = std::make_unique<rpp::params::ParameterHandler>(
        test_data_dir + "/test_component", *python_interpreter);

    ParametersDescription parameters
    {
        ParameterDescription::create<float>("float_var"),
        ParameterDescription::create<int>("int_var"),
        ParameterDescription::create<std::string>("str_var"),
        ParameterDescription::create<std::vector<int>>("list_var"),
        ParameterDescription::create<std::map<std::string, std::string>>("dict_var"),
        ParameterDescription::create<std::vector<std::map<std::string, std::string>>>("list_dict_var"),
        ParameterDescription::create<std::map<std::string, std::vector<int>>>("dict_list_var"),
        ParameterDescription::create<TestStruct1>("struct_var"),
        ParameterDescription::create<TestStruct2>("complext_struct_var"),
        ParameterDescription::create<TestStruct3>("non_dataclass_var"),

    };
    auto loaded_params = parameter_handler->load_parameters_from_python_module();
    std::unique_ptr<rpp::params::Parameters> params;
    rpp::params::ParameterHandler::resolve_params(parameters, loaded_params, params);

    try{
        params->get<float>("non_existent_param");
    }
    catch (const std::exception& e) {
        auto message = std::string(e.what());
        ASSERT_TRUE(message.find("Parameter 'non_existent_param' not found") != std::string::npos);
    }

    ASSERT_EQ(params->get<float>("float_var"), 5.0f);
    ASSERT_EQ(params->get<int>("int_var"), 1);
    ASSERT_EQ(params->get<std::string>("str_var"), "test");
    auto list = params->get<std::vector<int>>("list_var");
    ASSERT_EQ(list.size(), 3);
    ASSERT_EQ(list[0], 1);
    ASSERT_EQ(list[1], 2);
    ASSERT_EQ(list[2], 3);
    auto dict = params->get<std::map<std::string, std::string>>("dict_var");
    ASSERT_EQ(dict.size(), 1);
    ASSERT_EQ(dict["key"], "value");
    auto list_dict = params->get<std::vector<std::map<std::string, std::string>>>("list_dict_var");
    ASSERT_EQ(list_dict.size(), 2);
    ASSERT_EQ(list_dict[0]["key1"], "value1");
    ASSERT_EQ(list_dict[1]["key2"], "value2");
    auto dict_list = params->get<std::map<std::string, std::vector<int>>>("dict_list_var");
    ASSERT_EQ(dict_list.size(), 2);
    ASSERT_EQ(dict_list["key1"].size(), 3);
    ASSERT_EQ(dict_list["key1"][0], 1);
    ASSERT_EQ(dict_list["key1"][1], 2);
    ASSERT_EQ(dict_list["key1"][2], 3);
    ASSERT_EQ(dict_list["key2"].size(), 3);
    ASSERT_EQ(dict_list["key2"][0], 4);
    ASSERT_EQ(dict_list["key2"][1], 5);
    ASSERT_EQ(dict_list["key2"][2], 6);
    auto struct_var = params->get<TestStruct1>("struct_var");
    ASSERT_EQ(struct_var.width, 1);
    ASSERT_EQ(struct_var.height, "2");
    ASSERT_EQ(struct_var.fps, 3.0);
    auto struct_var2 = params->get<TestStruct2>("complext_struct_var");
    ASSERT_EQ(struct_var2.test_struct.width, 1);
    ASSERT_EQ(struct_var2.test_struct.height, "2");
    ASSERT_EQ(struct_var2.test_struct.fps, 3.0);
    ASSERT_EQ(struct_var2.test_struct_list.size(), 2);
    ASSERT_EQ(struct_var2.test_struct_list[0].width, 1);
    ASSERT_EQ(struct_var2.test_struct_list[0].height, "2");
    ASSERT_EQ(struct_var2.test_struct_list[0].fps, 3.0);
    ASSERT_EQ(struct_var2.test_struct_list[1].width, 2);
    ASSERT_EQ(struct_var2.test_struct_list[1].height, "3");
    ASSERT_EQ(struct_var2.test_struct_list[1].fps, 4.0);
    ASSERT_EQ(struct_var2.test_struct_map.size(), 2);
    ASSERT_EQ(struct_var2.test_struct_map["first"].width, 1);
    ASSERT_EQ(struct_var2.test_struct_map["first"].height, "2");
    ASSERT_EQ(struct_var2.test_struct_map["first"].fps, 3.0);
    ASSERT_EQ(struct_var2.test_struct_map["second"].width, 5);
    ASSERT_EQ(struct_var2.test_struct_map["second"].height, "6");
    ASSERT_EQ(struct_var2.test_struct_map["second"].fps, 7.0);
    auto non_dataclass_var = params->get<TestStruct3>("non_dataclass_var");
    ASSERT_EQ(non_dataclass_var.width, 1);
    ASSERT_EQ(non_dataclass_var.height, "2");
    ASSERT_EQ(non_dataclass_var.fps, 3.0);
    auto non_dataclass_dict_var = non_dataclass_var.dict_var;
    ASSERT_EQ(non_dataclass_dict_var.size(), 1);
    ASSERT_EQ(non_dataclass_dict_var["key"], "value");
    ASSERT_EQ(non_dataclass_var.test_struct.width, 1);
    ASSERT_EQ(non_dataclass_var.test_struct.height, "2");
    ASSERT_EQ(non_dataclass_var.test_struct.fps, 3.0);

}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    setenv("TEST_DATA_DIR", TEST_DATA_DIR, 1);

    // Pokrećemo GoogleTest najnormalnije
    return RUN_ALL_TESTS();
}
