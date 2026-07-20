from dataclasses import dataclass, field

@dataclass
class TestStruct:
    width: int = 1
    height: str = "2"
    fps: float = 3.0

@dataclass
class ComplextTestStruct:
    test_struct: TestStruct = TestStruct()
    test_struct_list: list = field(default_factory=lambda: [TestStruct(), TestStruct(width=2, height="3", fps=4.0)])
    test_struct_map: dict = field(default_factory=lambda: {"first": TestStruct(), "second": TestStruct(width=5, height="6", fps=7.0)})

class TestStructNondataclass:
    def __init__(self, width: int = 1, height: str = "2", fps: float = 3.0):
        self.width = width
        self.height = height
        self.fps = fps
        self.dict_var = {"key": "value"}
        self.test_struct = TestStruct()

class ComponentParams:
    float_var : float = 5.0
    int_var : int = 1
    str_var : str = "test"
    list_var : list = [1, 2, 3]
    dict_var : dict = {"key": "value"}
    list_dict_var : list = [{"key1": "value1"}, {"key2": "value2"}]
    dict_list_var : dict = {"key1": [1, 2, 3], "key2": [4, 5, 6]}
    struct_var : TestStruct = TestStruct()
    complext_struct_var : ComplextTestStruct = ComplextTestStruct()
    non_dataclass_var : TestStructNondataclass = TestStructNondataclass()

