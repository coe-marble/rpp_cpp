#pragma once
#include <string>
#include <variant>
#include <memory>
#include <functional>
#include <type_traits>
#include <capnp/message.h>

namespace rpp {

    // SFINAE Detector: Checks if type T defines the nested alias "IsRppStruct"
    template <typename T, typename = void> struct is_rpp_struct : std::false_type {};
    template <typename T> struct is_rpp_struct<T, std::void_t<typename T::IsRppStruct>> : std::true_type {};

    template <typename T, typename Enable = void>
    struct capnp_type_resolver {
        using type = T; // Fallback for primitives (double, int, bool)
    };

    template <typename T>
    struct capnp_type_resolver<T, std::enable_if_t<is_rpp_struct<T>::value>> {
        using type = typename T::CapnpType; // Used only for custom structure wrappers
    };

    // Shortcut alias for easy usage
    template <typename T>
    using capnp_type_resolver_t = typename capnp_type_resolver<T>::type;

    template <typename T>
    using El_T = capnp_type_resolver_t<T>;

    template <typename T, typename Enable> class List;


    // =========================================================================
    // 1. UNIFIED LISTCONST CLASS (For all Read-Only / Return contexts)
    // =========================================================================
    template <typename T, typename Enable = void>
    class ListConst {
    private:
        // Resolve underlying Cap'n Proto type: use T::CapnpType for wrappers, otherwise use raw T
        typename capnp::List<El_T<T>>::Reader list_reader_;
        std::shared_ptr<void> message_lifetime_; // Keeps the parent message alive for nested lists


    public:
        ListConst(typename capnp::List<El_T<T>>::Reader reader, std::shared_ptr<void> lifetime=nullptr)
            : list_reader_(reader), message_lifetime_(lifetime) {}
        size_t size() const { return list_reader_.size(); }
        bool empty() const { return list_reader_.size() == 0; }

        // Element accessor: returns T::Const for structures, or a raw value copy for primitives
        auto operator[](size_t index) const {
            if constexpr (is_rpp_struct<T>::value) {
                return typename T::Const(list_reader_[index]);
            } else {
                return list_reader_[index];
            }
        }

        operator typename capnp::List<El_T<T>>::Reader() const { return list_reader_; }
    };

    // Explicit specialization of ListConst for std::string
    template <>
    class ListConst<std::string> {
    private:
        capnp::List<capnp::Text>::Reader list_reader_;
        std::shared_ptr<void> message_lifetime_;
    public:
        ListConst(capnp::List<capnp::Text>::Reader reader, std::shared_ptr<void> lifetime=nullptr)
            : list_reader_(reader), message_lifetime_(lifetime) {}
        size_t size() const { return list_reader_.size(); }
        bool empty() const { return list_reader_.size() == 0; }
        std::string operator[](size_t index) const { return std::string(list_reader_[index].cStr()); }
        operator capnp::List<capnp::Text>::Reader() const { return list_reader_; }
    };


    // =========================================================================
    // 2. UNIFIED LIST CLASS (For all Writable / Builder contexts)
    // =========================================================================
    template <typename T, typename Enable = void>
    class List {
    private:
        typename capnp::List<El_T<T>>::Builder list_builder_;

        // Type definition for the parent structure resize callback
        using StructureCallback = std::function<typename capnp::List<El_T<T>>::Builder(size_t)>;

        // The variant storage for the allocator mechanism
        // SharedMessageBuilder is first, meaning List() = default; initializes with a standalone message.
        using SharedMessageBuilder = std::shared_ptr<capnp::MallocMessageBuilder>;
        std::variant<SharedMessageBuilder, StructureCallback> allocator_;

    public:
        // Proxy reference type to solve the 'lvalue required' error when assigning to primitives
        struct ElementRef {
        private:
            typename capnp::List<El_T<T>>::Builder& builder_;
            size_t index_;
        public:
            ElementRef(typename capnp::List<El_T<T>>::Builder& b, size_t i)
                : builder_(b), index_(i) {}
            ElementRef& operator=(const T& value) {
                if constexpr (is_rpp_struct<T>::value) {
                    builder_.setWithCaveats(index_, value.capnp_builder());
                } else {
                    builder_.set(index_, value);
                }
                return *this;

            }
            operator T() const { return builder_[index_]; }
        };

        // =========================================================================
        // CONSTRUCTOR 1: Empty constructor for standalone lists (not nested)
        // Inits its own MallocMessageBuilder via shared_ptr to keep data alive for readers
        // =========================================================================
        List()
            : allocator_(std::make_shared<capnp::MallocMessageBuilder>()) {}

        // =========================================================================
        // CONSTRUCTOR 2: For nested lists inside structures
        // Takes the raw capnp builder and a resize callback function from the parent
        // =========================================================================
        List(typename capnp::List<El_T<T>>::Builder list_builder, StructureCallback cb)
            : list_builder_(list_builder), allocator_(cb) {}

        // Standard move mechanics and copy prevention
        List(List&& other) noexcept = default;
        List& operator=(List&& other) noexcept = default;
        List(const List& other) = delete;

        // Core API methods
        size_t size() const { return list_builder_ ? list_builder_.size() : 0; }
        bool empty() const { return size() == 0; }

        void resize(size_t new_size) { init(new_size); }

        // Writable bracket operator: returns the mutable wrapper for structs, or the ElementRef proxy for primitives
        auto operator[](size_t index) {
            if constexpr (is_rpp_struct<T>::value) {
                return T(list_builder_[index]);
            } else {
                return ElementRef(list_builder_, index);
            }
        }

        // Const bracket operator: fallback read-only behavior when the list itself is const
        auto operator[](size_t index) const {
            if constexpr (is_rpp_struct<T>::value) {
                return typename T::Const(list_builder_.asReader()[index]);
            } else {
                return list_builder_.asReader()[index];
            }
        }
        // Implicit conversion operators for Cap'n Proto interop
        operator typename capnp::List<El_T<T>>::Builder() { return list_builder_; }
        operator typename capnp::List<El_T<T>>::Reader() const { return list_builder_.asReader(); }
        // Implicit conversion operator for  ListConst<T>
        operator ListConst<T>() const {
            if (std::holds_alternative<SharedMessageBuilder>(allocator_)) {
                if (auto& mb = std::get<SharedMessageBuilder>(allocator_)) {
                    return ListConst<T>(list_builder_.asReader(), mb);
                }
            }
            return ListConst<T>(list_builder_.asReader(), nullptr);
        }
    private:
        // The layout-agnostic initialization method triggered by .init() or .resize()
        void init(size_t new_size) {
            // Case 1: Standalone root list (posesses its own message builder)
            if (std::holds_alternative<SharedMessageBuilder>(allocator_)) {
                if (auto& mb = std::get<SharedMessageBuilder>(allocator_)) {
                    list_builder_ = mb->template initRoot<capnp::List<El_T<T>>>(new_size);
                }
            }
            // Case 2: Nested list field inside a structure (triggers parent allocation)
            else if (std::holds_alternative<StructureCallback>(allocator_)) {
                if (auto& cb = std::get<StructureCallback>(allocator_)) {
                    list_builder_ = cb(new_size);
                }
            }
        }

    };

    // Explicit specialization of List for std::string (Builder variant)
    template <>
    class List<std::string> {
    private:
        capnp::List<capnp::Text>::Builder list_builder_;
        using StructureCallback = std::function<capnp::List<capnp::Text>::Builder(size_t)>;
        std::variant<StructureCallback, capnp::MessageBuilder*> allocator_;

    public:
        struct ElementRef {
        private:
            capnp::List<capnp::Text>::Builder& builder_; size_t index_;
        public:
            ElementRef(capnp::List<capnp::Text>::Builder& b, size_t i)
                : builder_(b), index_(i) {}
            ElementRef& operator=(const std::string& value) { builder_.set(index_, value); return *this; }
            operator std::string() const { return std::string(builder_[index_].asReader().cStr()); }
        };

        List()
            : list_builder_(), allocator_(StructureCallback()) {}

        List(capnp::List<capnp::Text>::Builder list_builder, StructureCallback cb)
            : list_builder_(list_builder), allocator_(cb) {}
        List(capnp::List<capnp::Text>::Builder list_builder, capnp::MessageBuilder& mb)
            : list_builder_(list_builder), allocator_(&mb) {}

        size_t size() const { return list_builder_.size(); }
        bool empty() const { return list_builder_.size() == 0; }

        void init(size_t new_size) {
            if (std::holds_alternative<StructureCallback>(allocator_)) {
                list_builder_ = std::get<StructureCallback>(allocator_)(new_size);
            } else if (std::holds_alternative<capnp::MessageBuilder*>(allocator_)) {
                if (auto* mb = std::get<capnp::MessageBuilder*>(allocator_)) {
                    list_builder_ = mb->initRoot<capnp::List<capnp::Text>>(new_size);
                }
            }
        }
        void resize(size_t new_size) { init(new_size); }

        ElementRef operator[](size_t index) { return ElementRef(list_builder_, index); }
        std::string operator[](size_t index) const { return std::string(list_builder_.asReader()[index].cStr()); }

        operator capnp::List<capnp::Text>::Builder() { return list_builder_; }
        operator capnp::List<capnp::Text>::Reader() const { return list_builder_.asReader(); }
        operator ListConst<std::string>() const { return ListConst<std::string>(list_builder_.asReader()); }
    };

    class DataConst {
    private:
        const uint8_t* ptr_ = nullptr;
        size_t size_ = 0;
        std::shared_ptr<void> message_lifetime_;

    public:
        DataConst(capnp::Data::Reader reader, std::shared_ptr<void> lifetime = nullptr)
            : ptr_(reader.begin()), size_(reader.size()), message_lifetime_(lifetime) {}

        size_t size() const { return size_; }
        bool empty() const { return size_ == 0; }

        uint8_t operator[](size_t index) const { return ptr_[index]; }

        const uint8_t* begin() const { return ptr_; }
        const uint8_t* end() const { return ptr_ + size_; }

        operator capnp::Data::Reader() const { return capnp::Data::Reader(ptr_, size_); }
    };

    class Data {
    private:
        capnp::Data::Builder builder_;

        using ResizeCallback = std::function<capnp::Data::Builder(size_t)>;
        using AllocatorVariant = std::variant<ResizeCallback, std::shared_ptr<capnp::MallocMessageBuilder>>;
        AllocatorVariant allocator_;
    public:
        Data()
            : builder_(), allocator_(std::make_shared<capnp::MallocMessageBuilder>()) {}

        Data(capnp::Data::Builder builder, std::function<capnp::Data::Builder(size_t)> cb = nullptr)
            : builder_(builder), allocator_(cb) {}

        size_t size() const { return builder_.size(); }
        bool empty() const { return size() == 0; }

        uint8_t& operator[](size_t index) { return builder_[index]; }
        uint8_t operator[](size_t index) const { return builder_[index]; }

        // NOVO: Metoda za alokaciju memorije unutar poruke preko callbacka
        void resize(size_t new_size) { init(new_size); }

        operator capnp::Data::Builder() { return builder_; }
        operator capnp::Data::Reader() const { return builder_.asReader(); }
        operator DataConst() const { return DataConst(builder_.asReader()); }
    private:
        void init(size_t new_size) {
            if (std::holds_alternative<ResizeCallback>(allocator_)) {
                builder_ = std::get<ResizeCallback>(allocator_)(new_size);
            }
            else if (std::holds_alternative<std::shared_ptr<capnp::MallocMessageBuilder>>(allocator_)) {
                if (auto& mb = std::get<std::shared_ptr<capnp::MallocMessageBuilder>>(allocator_)) {
                    builder_ = mb->initRoot<capnp::Data>(new_size);
                }
            }
        }
    };

}
