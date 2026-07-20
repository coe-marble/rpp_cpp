#pragma once
// ============================================================================
// Public API
// ============================================================================

#define RPP_MEMBER(type, name, ...) \
    (type, name, __VA_ARGS__)



#define RPP_PARAM_STRUCT(name, ...)                         \
struct name                                                 \
{                                                           \
    RPP_DETAIL_FOR_EACH_(                                   \
        RPP_DETAIL_GENERATE_MEMBER_,                        \
        __VA_ARGS__                                         \
    )                                                       \
                                                            \
    template<typename Visitor>                              \
    void reflect(Visitor&& visitor)                         \
    {                                                       \
        RPP_DETAIL_FOR_EACH_(                               \
            RPP_DETAIL_GENERATE_REFLECT_,                  \
            __VA_ARGS__                                     \
        )                                                   \
    }                                                       \
                                                            \
    template<typename Visitor>                              \
    void reflect(Visitor&& visitor) const                   \
    {                                                       \
        RPP_DETAIL_FOR_EACH_(                               \
            RPP_DETAIL_GENERATE_REFLECT_,                  \
            __VA_ARGS__                                     \
        )                                                   \
    }                                                       \
                                                            \
    static constexpr const char* struct_name() { return #name; } \
};



// ============================================================================
// Implementation details
// ============================================================================


#define RPP_DETAIL_STRINGIZE_(x) \
    RPP_DETAIL_STRINGIZE_IMPL_(x)


#define RPP_DETAIL_STRINGIZE_IMPL_(x) \
    #x



// --------------------------------------------------------------------------
// Tuple extraction
// --------------------------------------------------------------------------

#define RPP_DETAIL_FIELD_TYPE_(field) \
    RPP_DETAIL_FIELD_TYPE_IMPL_ field


#define RPP_DETAIL_FIELD_TYPE_IMPL_(type, name, ...) \
    type



#define RPP_DETAIL_FIELD_NAME_(field) \
    RPP_DETAIL_FIELD_NAME_IMPL_ field


#define RPP_DETAIL_FIELD_NAME_IMPL_(type, name, ...) \
    name



#define RPP_DETAIL_FIELD_DEFAULT_(field) \
    RPP_DETAIL_FIELD_DEFAULT_IMPL_ field


#define RPP_DETAIL_FIELD_DEFAULT_IMPL_(type, name, ...) \
    __VA_ARGS__



// --------------------------------------------------------------------------
// Generate members
// --------------------------------------------------------------------------

#define RPP_DETAIL_GENERATE_MEMBER_(field)                  \
    RPP_DETAIL_FIELD_TYPE_(field)                           \
    RPP_DETAIL_FIELD_NAME_(field) =                         \
    RPP_DETAIL_FIELD_DEFAULT_(field);



// --------------------------------------------------------------------------
// Generate reflection
// --------------------------------------------------------------------------

#define RPP_DETAIL_GENERATE_REFLECT_(field)                 \
    visitor(                                                \
        RPP_DETAIL_STRINGIZE_(                             \
            RPP_DETAIL_FIELD_NAME_(field)                   \
        ),                                                  \
        RPP_DETAIL_FIELD_NAME_(field)                       \
    );



// --------------------------------------------------------------------------
// FOR_EACH implementation
// Supports up to 16 fields
// --------------------------------------------------------------------------

#define RPP_DETAIL_FE_1_(M,a) \
    M(a)

#define RPP_DETAIL_FE_2_(M,a,b) \
    M(a) M(b)

#define RPP_DETAIL_FE_3_(M,a,b,c) \
    M(a) M(b) M(c)

#define RPP_DETAIL_FE_4_(M,a,b,c,d) \
    M(a) M(b) M(c) M(d)

#define RPP_DETAIL_FE_5_(M,a,b,c,d,e) \
    M(a) M(b) M(c) M(d) M(e)

#define RPP_DETAIL_FE_6_(M,a,b,c,d,e,f) \
    M(a) M(b) M(c) M(d) M(e) M(f)

#define RPP_DETAIL_FE_7_(M,a,b,c,d,e,f,g) \
    M(a) M(b) M(c) M(d) M(e) M(f) M(g)

#define RPP_DETAIL_FE_8_(M,a,b,c,d,e,f,g,h) \
    M(a) M(b) M(c) M(d) M(e) M(f) M(g) M(h)


#define RPP_DETAIL_GET_FE_( \
    _1,_2,_3,_4,_5,_6,_7,_8,NAME,...) NAME


#define RPP_DETAIL_FOR_EACH_(M,...)                         \
    RPP_DETAIL_GET_FE_(                                     \
        __VA_ARGS__,                                       \
        RPP_DETAIL_FE_8_,                                   \
        RPP_DETAIL_FE_7_,                                   \
        RPP_DETAIL_FE_6_,                                   \
        RPP_DETAIL_FE_5_,                                   \
        RPP_DETAIL_FE_4_,                                   \
        RPP_DETAIL_FE_3_,                                   \
        RPP_DETAIL_FE_2_,                                   \
        RPP_DETAIL_FE_1_                                    \
    )(M,__VA_ARGS__)
