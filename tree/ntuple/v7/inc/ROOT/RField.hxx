/// \file ROOT/RField.hxx
/// \ingroup NTuple ROOT7
/// \author Jakob Blomer <jblomer@cern.ch>
/// \date 2018-10-09
/// \warning This is part of the ROOT 7 prototype! It will change without notice. It might trigger earthquakes. Feedback
/// is welcome!

/*************************************************************************
 * Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT7_RField
#define ROOT7_RField

#include <ROOT/RColumn.hxx>
#include <ROOT/RError.hxx>
#include <ROOT/RColumnElement.hxx>
#include <ROOT/RNTupleUtil.hxx>
#include <ROOT/RSpan.hxx>
#include <string_view>
#include <ROOT/RVec.hxx>
#include <ROOT/TypeTraits.hxx>

#include <TGenericClassInfo.h>
#include <TVirtualCollectionProxy.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <variant>
#include <vector>
#include <utility>

class TClass;
class TEnum;

namespace ROOT {

class TSchemaRule;

namespace Experimental {

class RCollectionField;
class RCollectionNTupleWriter;
class REntry;
class RNTupleModel;

namespace Internal {
struct RFieldCallbackInjector;
} // namespace Internal

namespace Detail {

class RFieldVisitor;
class RPageStorage;

// clang-format off
/**
\class ROOT::Experimental::Detail::RFieldBase
\ingroup NTuple
\brief A field translates read and write calls from/to underlying columns to/from tree values

A field is a serializable C++ type or a container for a collection of sub fields. The RFieldBase and its
type-safe descendants provide the object to column mapper. They map C++ objects to primitive columns.  The
mapping is trivial for simple types such as 'double'. Complex types resolve to multiple primitive columns.
The field knows based on its type and the field name the type(s) and name(s) of the columns.
*/
// clang-format on
class RFieldBase {
   friend class ROOT::Experimental::RCollectionField; // to move the fields from the collection model
   friend struct ROOT::Experimental::Internal::RFieldCallbackInjector; // used for unit tests
   using ReadCallback_t = std::function<void(void *)>;

public:
   static constexpr std::uint32_t kInvalidTypeVersion = -1U;
   /// No constructor needs to be called, i.e. any bit pattern in the allocated memory represents a valid type
   /// A trivially constructible field has a no-op GenerateValue() implementation
   static constexpr int kTraitTriviallyConstructible = 0x01;
   /// The type is cleaned up just by freeing its memory. I.e. DestroyValue() is a no-op.
   static constexpr int kTraitTriviallyDestructible = 0x02;
   /// A field of a fundamental type that can be directly mapped via `RField<T>::Map()`, i.e. maps as-is to a single
   /// column
   static constexpr int kTraitMappable = 0x04;
   /// Shorthand for types that are both trivially constructible and destructible
   static constexpr int kTraitTrivialType = kTraitTriviallyConstructible | kTraitTriviallyDestructible;

   using ColumnRepresentation_t = std::vector<EColumnType>;

   /// During its lifetime, a field undergoes the following possible state transitions:
   ///
   ///  [*] --> Unconnected --> ConnectedToSink ----
   ///               |      |                      |
   ///               |      --> ConnectedToSource ---> [*]
   ///               |                             |
   ///               -------------------------------
   enum class EState { kUnconnected, kConnectedToSink, kConnectedToSource };

   /// Some fields have multiple possible column representations, e.g. with or without split encoding.
   /// All column representations supported for writing also need to be supported for reading. In addition,
   /// fields can support extra column representations for reading only, e.g. a 64bit integer reading from a
   /// 32bit column.
   /// The defined column representations must be supported by corresponding column packing/unpacking implementations,
   /// i.e. for the example above, the unpacking of 32bit ints to 64bit pages must be implemented in RColumnElement.hxx
   class RColumnRepresentations {
   public:
      using TypesList_t = std::vector<ColumnRepresentation_t>;
      RColumnRepresentations();
      RColumnRepresentations(const TypesList_t &serializationTypes, const TypesList_t &deserializationExtraTypes);

      /// The first column list from fSerializationTypes is the default for writing.
      const ColumnRepresentation_t &GetSerializationDefault() const { return fSerializationTypes[0]; }
      const TypesList_t &GetSerializationTypes() const { return fSerializationTypes; }
      const TypesList_t &GetDeserializationTypes() const { return fDeserializationTypes; }

   private:
      TypesList_t fSerializationTypes;
      /// The union of the serialization types and the deserialization extra types.  Duplicates the serialization types
      /// list but the benenfit is that GetDeserializationTypes does not need to compile the list.
      TypesList_t fDeserializationTypes;
   }; // class RColumnRepresentations

   /// Points to an object with RNTuple I/O support and keeps a pointer to the corresponding field.
   /// Only fields can create RValue objects through generation, binding or splitting.
   /// An RValue object can be owning or non-owning. Only RField::GenerateValue creates owning RValues.
   /// Owning RValues destroy and free the object upon destruction.
   class RValue {
      friend class RFieldBase;

   private:
      RFieldBase *fField = nullptr; ///< The field that created the RValue
      /// Created by RFieldBase::GenerateValue() or a non-owning pointer from SplitValue() or BindValue()
      void *fObjPtr = nullptr;
      bool fIsOwning = false; ///< If true, fObjPtr is destroyed in the destructor

      RValue(RFieldBase *field, void *objPtr, bool isOwning) : fField(field), fObjPtr(objPtr), fIsOwning(isOwning) {}

      void DestroyIfOwning()
      {
         if (fIsOwning)
            fField->DestroyValue(fObjPtr);
      }

   public:
      RValue(const RValue &) = delete;
      RValue &operator=(const RValue &) = delete;
      RValue(RValue &&other) : fField(other.fField), fObjPtr(other.fObjPtr) { std::swap(fIsOwning, other.fIsOwning); }
      RValue &operator=(RValue &&other)
      {
         DestroyIfOwning();
         fIsOwning = false;
         std::swap(fField, other.fField);
         std::swap(fObjPtr, other.fObjPtr);
         std::swap(fIsOwning, other.fIsOwning);
         return *this;
      }
      ~RValue() { DestroyIfOwning(); }

      RValue GetNonOwningCopy() { return RValue(fField, fObjPtr, false); }

      template <typename T>
      void *Release()
      {
         fIsOwning = false;
         void *result = nullptr;
         std::swap(result, fObjPtr);
         return static_cast<T *>(result);
      }

      std::size_t Append() { return fField->Append(fObjPtr); }
      void Read(NTupleSize_t globalIndex) { fField->Read(globalIndex, fObjPtr); }
      void Read(const RClusterIndex &clusterIndex) { fField->Read(clusterIndex, fObjPtr); }

      template <typename T>
      T *Get() const
      {
         return static_cast<T *>(fObjPtr);
      }
      void *GetRawPtr() const { return fObjPtr; }
      RFieldBase *GetField() const { return fField; }
   }; // class RValue

   /// Similar to RValue but manages an array of consecutive values. Bulks have to come from the same cluster.
   /// Bulk I/O works with two bit masks: the mask of all the available entries in the current bulk and the mask
   /// of the required entries in a bulk read. The idea is that a single bulk may serve multiple read operations
   /// on the same range, where in each read operation a different subset of values is required.
   /// The memory of the value array is managed by the RBulk class.
   class RBulk {
   private:
      friend class RFieldBase;

      RFieldBase *fField = nullptr;       ///< The field that created the array of values
      void *fValues = nullptr;            ///< Pointer to the start of the array
      std::size_t fValueSize = 0;         ///< Cached copy of fField->GetValueSize()
      std::size_t fCapacity = 0;          ///< The size of the array memory block in number of values
      std::size_t fSize = 0;              ///< The number of available values in the array (provided their mask is set)
      std::unique_ptr<bool[]> fMaskAvail; ///< Masks invalid values in the array
      std::size_t fNValidValues = 0;      ///< The sum of non-zero elements in the fMask
      RClusterIndex fFirstIndex;          ///< Index of the first value of the array
      /// Reading arrays of complex values may require additional memory, for instance for the elements of
      /// arrays of vectors. A pointer to the fAuxData array is passed to the field's BulkRead method.
      /// The RBulk class does not modify the array in-between calls to the field's BulkRead method.
      std::vector<unsigned char> fAuxData;

      void ReleaseValues();
      /// Sets a new range for the bulk. If there is enough capacity, the fValues array will be reused.
      /// Otherwise a new array is allocated. After reset, fMaskAvail is false for all values.
      void Reset(const RClusterIndex &firstIndex, std::size_t size);
      void CountValidValues();

      bool ContainsRange(const RClusterIndex &firstIndex, std::size_t size) const
      {
         if (firstIndex.GetClusterId() != fFirstIndex.GetClusterId())
            return false;
         return (firstIndex.GetIndex() >= fFirstIndex.GetIndex()) &&
                ((firstIndex.GetIndex() + size) <= (fFirstIndex.GetIndex() + fSize));
      }

      void *GetValuePtrAt(std::size_t idx) const
      {
         return reinterpret_cast<unsigned char *>(fValues) + idx * fValueSize;
      }

      explicit RBulk(RFieldBase *field) : fField(field), fValueSize(field->GetValueSize()) {}

   public:
      ~RBulk();
      RBulk(const RBulk &) = delete;
      RBulk &operator=(const RBulk &) = delete;
      RBulk(RBulk &&other);
      RBulk &operator=(RBulk &&other);

      /// Reads 'size' values from the associated field, starting from 'firstIndex'. Note that the index is given
      /// relative to a certain cluster. The return value points to the array of read objects.
      /// The 'maskReq' parameter is a bool array of at least 'size' elements. Only objects for which the mask is
      /// true are guaranteed to be read in the returned value array.
      void *ReadBulk(const RClusterIndex &firstIndex, const bool *maskReq, std::size_t size)
      {
         if (!ContainsRange(firstIndex, size))
            Reset(firstIndex, size);

         // We may read a sub range of the currently available range
         auto offset = firstIndex.GetIndex() - fFirstIndex.GetIndex();

         if (fNValidValues == fSize)
            return GetValuePtrAt(offset);

         RBulkSpec bulkSpec;
         bulkSpec.fFirstIndex = firstIndex;
         bulkSpec.fCount = size;
         bulkSpec.fMaskReq = maskReq;
         bulkSpec.fMaskAvail = &fMaskAvail[offset];
         bulkSpec.fValues = GetValuePtrAt(offset);
         bulkSpec.fAuxData = &fAuxData;
         auto nRead = fField->ReadBulk(bulkSpec);
         if (nRead == RBulkSpec::kAllSet) {
            if ((offset == 0) && (size == fSize)) {
               fNValidValues = fSize;
            } else {
               CountValidValues();
            }
         } else {
            fNValidValues += nRead;
         }
         return GetValuePtrAt(offset);
      }
   }; // class RBulk

private:
   /// The field name relative to its parent field
   std::string fName;
   /// The C++ type captured by this field
   std::string fType;
   /// The role of this field in the data model structure
   ENTupleStructure fStructure;
   /// For fixed sized arrays, the array length
   std::size_t fNRepetitions;
   /// A field qualifies as simple if it is both mappable and has no post-read callback
   bool fIsSimple;
   /// When the columns are connected to a page source or page sink, the field represents a field id in the
   /// corresponding RNTuple descriptor. This on-disk ID is set in RPageSink::Create() for writing and by
   /// RFieldDescriptor::CreateField() when recreating a field / model from the stored descriptor.
   DescriptorId_t fOnDiskId = kInvalidDescriptorId;
   /// Free text set by the user
   std::string fDescription;
   /// Changed by ConnectTo[Sink,Source], reset by Clone()
   EState fState = EState::kUnconnected;

   void InvokeReadCallbacks(void *target)
   {
      for (const auto &func : fReadCallbacks)
         func(target);
   }

   /// Translate an entry index to a column element index of the principal column and viceversa.  These functions
   /// take into account the role and number of repetitions on each level of the field hierarchy as follows:
   /// - Top level fields: element index == entry index
   /// - Record fields propagate their principal column index to the principal columns of direct descendant fields
   /// - Collection and variant fields set the principal column index of their childs to 0
   ///
   /// The column element index also depends on the number of repetitions of each field in the hierarchy, e.g., given a
   /// field with type `std::array<std::array<float, 4>, 2>`, this function returns 8 for the inner-most field.
   NTupleSize_t EntryToColumnElementIndex(NTupleSize_t globalIndex) const;

protected:
   /// Input parameter to ReadBulk() and ReadBulkImpl(). See RBulk class for more information
   struct RBulkSpec {
      /// As a return value of ReadBulk and ReadBulkImpl(), indicates that the full bulk range was read
      /// independent of the provided masks.
      static const std::size_t kAllSet = std::size_t(-1);

      RClusterIndex fFirstIndex; ///< Start of the bulk range
      std::size_t fCount = 0;    ///< Size of the bulk range
      /// A bool array of size fCount, indicating the required values in the requested range
      const bool *fMaskReq = nullptr;
      bool *fMaskAvail = nullptr; ///< A bool array of size fCount, indicating the valid values in fValues
      /// The destination area, which has to be a big enough array of valid objects of the correct type
      void *fValues = nullptr;
      /// Reference to memory owned by the RBulk class. The field implementing BulkReadImpl may use fAuxData
      /// as memory that stays persistent between calls.
      std::vector<unsigned char> *fAuxData = nullptr;
   };

   /// Collections and classes own sub fields
   std::vector<std::unique_ptr<RFieldBase>> fSubFields;
   /// Sub fields point to their mother field
   RFieldBase* fParent;
   /// Points into fColumns.  All fields that have columns have a distinct main column. For simple fields
   /// (float, int, ...), the principal column corresponds to the field type. For collection fields expect std::array,
   /// the main column is the offset field.  Class fields have no column of their own.
   RColumn* fPrincipalColumn;
   /// The columns are connected either to a sink or to a source (not to both); they are owned by the field.
   std::vector<std::unique_ptr<RColumn>> fColumns;
   /// Properties of the type that allow for optimizations of collections of that type
   int fTraits = 0;
   /// A typedef or using name that was used when creating the field
   std::string fTypeAlias;
   /// List of functions to be called after reading a value
   std::vector<ReadCallback_t> fReadCallbacks;
   /// C++ type version cached from the descriptor after a call to `ConnectPageSource()`
   std::uint32_t fOnDiskTypeVersion = kInvalidTypeVersion;
   /// Points into the static vector GetColumnRepresentations().GetSerializationTypes() when SetColumnRepresentative
   /// is called.  Otherwise GetColumnRepresentative returns the default representation.
   const ColumnRepresentation_t *fColumnRepresentative = nullptr;

   /// Implementations in derived classes should return a static RColumnRepresentations object. The default
   /// implementation does not attach any columns to the field.
   virtual const RColumnRepresentations &GetColumnRepresentations() const;
   /// Creates the backing columns corresponsing to the field type for writing
   virtual void GenerateColumnsImpl() = 0;
   /// Creates the backing columns corresponsing to the field type for reading.
   /// The method should to check, using the page source and fOnDiskId, if the column types match
   /// and throw if they don't.
   virtual void GenerateColumnsImpl(const RNTupleDescriptor &desc) = 0;
   /// Returns the on-disk column types found in the provided descriptor for fOnDiskId. Throws an exception if the types
   /// don't match any of the deserialization types from GetColumnRepresentations().
   const ColumnRepresentation_t &EnsureCompatibleColumnTypes(const RNTupleDescriptor &desc) const;
   /// When connecting a field to a page sink, the field's default column representation is subject
   /// to adjustment according to the write options. E.g., if compression is turned off, encoded columns
   /// are changed to their unencoded counterparts.
   void AutoAdjustColumnTypes(const RNTupleWriteOptions &options);

   /// Called by Clone(), which additionally copies the on-disk ID
   virtual std::unique_ptr<RFieldBase> CloneImpl(std::string_view newName) const = 0;

   /// Constructs value in a given location of size at least GetValueSize(). Called by the base class' GenerateValue().
   virtual void GenerateValue(void *where) const = 0;
   /// Releases the resources acquired during GenerateValue (memory and constructor)
   /// This implementation works for types with a trivial destructor and should be overwritten otherwise.
   virtual void DestroyValue(void *objPtr, bool dtorOnly = false) const;
   /// Allow derived classes to call GenerateValue(void *) and DestroyValue on other (sub) fields.
   static void CallGenerateValueOn(const RFieldBase &other, void *where) { other.GenerateValue(where); }
   static void CallDestroyValueOn(const RFieldBase &other, void *objPtr, bool dtorOnly = false)
   {
      other.DestroyValue(objPtr, dtorOnly);
   }

   /// Operations on values of complex types, e.g. ones that involve multiple columns or for which no direct
   /// column type exists.
   virtual std::size_t AppendImpl(const void *from);
   virtual void ReadGlobalImpl(NTupleSize_t globalIndex, void *to);
   virtual void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to)
   {
      ReadGlobalImpl(fPrincipalColumn->GetGlobalIndex(clusterIndex), to);
   }

   /// Write the given value into columns. The value object has to be of the same type as the field.
   /// Returns the number of uncompressed bytes written.
   std::size_t Append(const void *from)
   {
      if (~fTraits & kTraitMappable)
         return AppendImpl(from);

      fPrincipalColumn->Append(from);
      return fPrincipalColumn->GetElement()->GetPackedSize();
   }

   /// Populate a single value with data from the field. The memory location pointed to by to needs to be of the
   /// fitting type. The fast path is conditioned by the field qualifying as simple, i.e. maps as-is
   /// to a single column and has no read callback.
   void Read(NTupleSize_t globalIndex, void *to)
   {
      if (fIsSimple)
         return (void)fPrincipalColumn->Read(globalIndex, to);

      if (fTraits & kTraitMappable)
         fPrincipalColumn->Read(globalIndex, to);
      else
         ReadGlobalImpl(globalIndex, to);
      if (R__unlikely(!fReadCallbacks.empty()))
         InvokeReadCallbacks(to);
   }

   void Read(const RClusterIndex &clusterIndex, void *to)
   {
      if (fIsSimple)
         return (void)fPrincipalColumn->Read(clusterIndex, to);

      if (fTraits & kTraitMappable)
         fPrincipalColumn->Read(clusterIndex, to);
      else
         ReadInClusterImpl(clusterIndex, to);
      if (R__unlikely(!fReadCallbacks.empty()))
         InvokeReadCallbacks(to);
   }

   /// General implementation of bulk read. Loop over the required range and read values that are required
   /// and not already present. Derived classes may implement more optimized versions of this method.
   /// See ReadBulk() for the return value.
   virtual std::size_t ReadBulkImpl(const RBulkSpec &bulkSpec);

   /// Returns the number of newly available values, that is the number of bools in bulkSpec.fMaskAvail that
   /// flipped from false to true. As a special return value, kAllSet can be used if all values are read
   /// independent from the masks.
   std::size_t ReadBulk(const RBulkSpec &bulkSpec)
   {
      if (fIsSimple) {
         /// For simple types, ignore the mask and memcopy the values into the destination
         fPrincipalColumn->ReadV(bulkSpec.fFirstIndex, bulkSpec.fCount, bulkSpec.fValues);
         std::fill(bulkSpec.fMaskAvail, bulkSpec.fMaskAvail + bulkSpec.fCount, true);
         return RBulkSpec::kAllSet;
      }

      return ReadBulkImpl(bulkSpec);
   }

   /// Allow derived classes to call Append and Read on other (sub) fields.
   static std::size_t CallAppendOn(RFieldBase &other, const void *from) { return other.Append(from); }
   static void CallReadOn(RFieldBase &other, const RClusterIndex &clusterIndex, void *to)
   {
      other.Read(clusterIndex, to);
   }
   static void CallReadOn(RFieldBase &other, NTupleSize_t globalIndex, void *to) { other.Read(globalIndex, to); }

   /// Fields may need direct access to the principal column of their sub fields, e.g. in RRVecField::ReadBulk
   static RColumn *GetPrincipalColumnOf(const RFieldBase &other) { return other.fPrincipalColumn; }

   /// Set a user-defined function to be called after reading a value, giving a chance to inspect and/or modify the
   /// value object.
   /// Returns an index that can be used to remove the callback.
   size_t AddReadCallback(ReadCallback_t func);
   void RemoveReadCallback(size_t idx);

   // Perform housekeeping tasks for global to cluster-local index translation
   virtual void CommitClusterImpl() {}

   /// Add a new subfield to the list of nested fields
   void Attach(std::unique_ptr<Detail::RFieldBase> child);

   /// Called by `ConnectPageSource()` only once connected; derived classes may override this
   /// as appropriate
   virtual void OnConnectPageSource() {}

   /// Factory method to resurrect a field from the stored on-disk type information.  This overload takes an already
   /// normalized type name and type alias
   /// TODO(jalopezg): this overload may eventually be removed leaving only the `RFieldBase::Create()` that takes a
   /// single type name
   static RResult<std::unique_ptr<RFieldBase>>
   Create(const std::string &fieldName, const std::string &canonicalType, const std::string &typeAlias);

public:
   /// Iterates over the sub tree of fields in depth-first search order
   template <bool IsConstT>
   class RSchemaIteratorTemplate {
   private:
      struct Position {
         using FieldPtr_t = std::conditional_t<IsConstT, const RFieldBase *, RFieldBase *>;
         Position() : fFieldPtr(nullptr), fIdxInParent(-1) { }
         Position(FieldPtr_t fieldPtr, int idxInParent) : fFieldPtr(fieldPtr), fIdxInParent(idxInParent) {}
         FieldPtr_t fFieldPtr;
         int fIdxInParent;
      };
      /// The stack of nodes visited when walking down the tree of fields
      std::vector<Position> fStack;
   public:
      using iterator = RSchemaIteratorTemplate<IsConstT>;
      using iterator_category = std::forward_iterator_tag;
      using difference_type = std::ptrdiff_t;
      using value_type = std::conditional_t<IsConstT, const RFieldBase, RFieldBase>;
      using pointer = std::conditional_t<IsConstT, const RFieldBase *, RFieldBase *>;
      using reference = std::conditional_t<IsConstT, const RFieldBase &, RFieldBase &>;

      RSchemaIteratorTemplate() { fStack.emplace_back(Position()); }
      RSchemaIteratorTemplate(pointer val, int idxInParent) { fStack.emplace_back(Position(val, idxInParent)); }
      ~RSchemaIteratorTemplate() {}
      /// Given that the iterator points to a valid field which is not the end iterator, go to the next field
      /// in depth-first search order
      void Advance()
      {
         auto itr = fStack.rbegin();
         if (!itr->fFieldPtr->fSubFields.empty()) {
            fStack.emplace_back(Position(itr->fFieldPtr->fSubFields[0].get(), 0));
            return;
         }

         unsigned int nextIdxInParent = ++(itr->fIdxInParent);
         while (nextIdxInParent >= itr->fFieldPtr->fParent->fSubFields.size()) {
            if (fStack.size() == 1) {
               itr->fFieldPtr = itr->fFieldPtr->fParent;
               itr->fIdxInParent = -1;
               return;
            }
            fStack.pop_back();
            itr = fStack.rbegin();
            nextIdxInParent = ++(itr->fIdxInParent);
         }
         itr->fFieldPtr = itr->fFieldPtr->fParent->fSubFields[nextIdxInParent].get();
      }

      iterator  operator++(int) /* postfix */        { auto r = *this; Advance(); return r; }
      iterator& operator++()    /* prefix */         { Advance(); return *this; }
      reference operator* () const                   { return *fStack.back().fFieldPtr; }
      pointer   operator->() const                   { return fStack.back().fFieldPtr; }
      bool      operator==(const iterator& rh) const { return fStack.back().fFieldPtr == rh.fStack.back().fFieldPtr; }
      bool      operator!=(const iterator& rh) const { return fStack.back().fFieldPtr != rh.fStack.back().fFieldPtr; }
   };
   using RSchemaIterator = RSchemaIteratorTemplate<false>;
   using RConstSchemaIterator = RSchemaIteratorTemplate<true>;

   /// The constructor creates the underlying column objects and connects them to either a sink or a source.
   /// If `isSimple` is `true`, the trait `kTraitMappable` is automatically set on construction. However, the
   /// field might be demoted to non-simple if a post-read callback is set.
   RFieldBase(std::string_view name, std::string_view type, ENTupleStructure structure, bool isSimple,
              std::size_t nRepetitions = 0);
   RFieldBase(const RFieldBase&) = delete;
   RFieldBase(RFieldBase&&) = default;
   RFieldBase& operator =(const RFieldBase&) = delete;
   RFieldBase& operator =(RFieldBase&&) = default;
   virtual ~RFieldBase();

   /// Copies the field and its sub fields using a possibly new name and a new, unconnected set of columns
   std::unique_ptr<RFieldBase> Clone(std::string_view newName) const;

   /// Factory method to resurrect a field from the stored on-disk type information
   static RResult<std::unique_ptr<RFieldBase>>
   Create(const std::string &fieldName, const std::string &typeName);
   /// Check whether a given string is a valid field name
   static RResult<void> EnsureValidFieldName(std::string_view fieldName);

   /// Generates an object of the field type and allocates new initialized memory according to the type.
   RValue GenerateValue();
   /// The returned bulk is initially empty; RBulk::ReadBulk will construct the array of values
   RBulk GenerateBulk() { return RBulk(this); }
   /// Creates a value from a memory location with an already constructed object
   RValue BindValue(void *where) { return RValue(this, where, false /* isOwning */); }
   /// Creates the list of direct child values given a value for this field.  E.g. a single value for the
   /// correct variant or all the elements of a collection.  The default implementation assumes no sub values
   /// and returns an empty vector.
   virtual std::vector<RValue> SplitValue(const RValue &value) const;
   /// The number of bytes taken by a value of the appropriate type
   virtual size_t GetValueSize() const = 0;
   /// As a rule of thumb, the alignment is equal to the size of the type. There are, however, various exceptions
   /// to this rule depending on OS and CPU architecture. So enforce the alignment to be explicitly spelled out.
   virtual size_t GetAlignment() const = 0;
   int GetTraits() const { return fTraits; }
   bool HasReadCallbacks() const { return !fReadCallbacks.empty(); }

   /// Flushes data from active columns to disk and calls CommitClusterImpl
   void CommitCluster();

   std::string GetName() const { return fName; }
   /// Returns the field name and parent field names separated by dots ("grandparent.parent.child")
   std::string GetQualifiedFieldName() const;
   std::string GetType() const { return fType; }
   std::string GetTypeAlias() const { return fTypeAlias; }
   ENTupleStructure GetStructure() const { return fStructure; }
   std::size_t GetNRepetitions() const { return fNRepetitions; }
   NTupleSize_t GetNElements() const { return fPrincipalColumn->GetNElements(); }
   RFieldBase *GetParent() const { return fParent; }
   std::vector<RFieldBase *> GetSubFields() const;
   bool IsSimple() const { return fIsSimple; }
   /// Get the field's description
   std::string GetDescription() const { return fDescription; }
   void SetDescription(std::string_view description);
   EState GetState() const { return fState; }

   DescriptorId_t GetOnDiskId() const { return fOnDiskId; }
   void SetOnDiskId(DescriptorId_t id);

   /// Returns the fColumnRepresentative pointee or, if unset, the field's default representative
   const ColumnRepresentation_t &GetColumnRepresentative() const;
   /// Fixes a column representative. This can only be done _before_ connecting the field to a page sink.
   /// Otherwise, or if the provided representation is not in the list of GetColumnRepresentations,
   /// an exception is thrown
   void SetColumnRepresentative(const ColumnRepresentation_t &representative);
   /// Whether or not an explicit column representative was set
   bool HasDefaultColumnRepresentative() const { return fColumnRepresentative == nullptr; }

   /// Fields and their columns live in the void until connected to a physical page storage.  Only once connected, data
   /// can be read or written.  In order to find the field in the page storage, the field's on-disk ID has to be set.
   /// \param firstEntry The global index of the first entry with on-disk data for the connected field
   void ConnectPageSink(RPageSink &pageSink, NTupleSize_t firstEntry = 0);
   void ConnectPageSource(RPageSource &pageSource);

   /// Indicates an evolution of the mapping scheme from C++ type to columns
   virtual std::uint32_t GetFieldVersion() const { return 0; }
   /// Indicates an evolution of the C++ type itself
   virtual std::uint32_t GetTypeVersion() const { return 0; }
   /// Return the C++ type version stored in the field descriptor; only valid after a call to `ConnectPageSource()`
   std::uint32_t GetOnDiskTypeVersion() const { return fOnDiskTypeVersion; }

   RSchemaIterator begin()
   {
      return fSubFields.empty() ? RSchemaIterator(this, -1) : RSchemaIterator(fSubFields[0].get(), 0);
   }
   RSchemaIterator end() { return RSchemaIterator(this, -1); }
   RConstSchemaIterator cbegin() const
   {
      return fSubFields.empty() ? RConstSchemaIterator(this, -1) : RConstSchemaIterator(fSubFields[0].get(), 0);
   }
   RConstSchemaIterator cend() const { return RConstSchemaIterator(this, -1); }

   virtual void AcceptVisitor(RFieldVisitor &visitor) const;
};

} // namespace Detail



/// The container field for an ntuple model, which itself has no physical representation.
/// Therefore, the zero field must not be connected to a page source or sink.
class RFieldZero : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;
   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor &) final {}
   void GenerateValue(void *) const final {}

public:
   RFieldZero() : Detail::RFieldBase("", "", ENTupleStructure::kRecord, false /* isSimple */) { }

   using Detail::RFieldBase::Attach;
   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return 0; }
   size_t GetAlignment() const final { return 0; }

   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

/// The field for a class with dictionary
class RClassField : public Detail::RFieldBase {
private:
   enum ESubFieldRole {
      kBaseClass,
      kDataMember,
   };
   struct RSubFieldInfo {
      ESubFieldRole fRole;
      std::size_t fOffset;
   };
   /// Prefix used in the subfield names generated for base classes
   static constexpr const char *kPrefixInherited{":"};

   TClass* fClass;
   /// Additional information kept for each entry in `fSubFields`
   std::vector<RSubFieldInfo> fSubFieldsInfo;
   std::size_t fMaxAlignment = 1;

private:
   RClassField(std::string_view fieldName, std::string_view className, TClass *classp);
   void Attach(std::unique_ptr<Detail::RFieldBase> child, RSubFieldInfo info);
   /// Register post-read callbacks corresponding to a list of ROOT I/O customization rules. `classp` is used to
   /// fill the `TVirtualObject` instance passed to the user function.
   void AddReadCallbacksFromIORules(const std::span<const TSchemaRule *> rules, TClass *classp = nullptr);

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;
   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor &) final {}

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;
   void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to) final;
   void OnConnectPageSource() final;

public:
   RClassField(std::string_view fieldName, std::string_view className);
   RClassField(RClassField&& other) = default;
   RClassField& operator =(RClassField&& other) = default;
   ~RClassField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const override;
   size_t GetAlignment() const final { return fMaxAlignment; }
   std::uint32_t GetTypeVersion() const final;
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const override;
};

/// The field for an unscoped or scoped enum with dictionary
class REnumField : public Detail::RFieldBase {
private:
   REnumField(std::string_view fieldName, std::string_view enumName, TEnum *enump);
   REnumField(std::string_view fieldName, std::string_view enumName, std::unique_ptr<RFieldBase> intField);

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;
   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor & /* desc */) final {}

   void GenerateValue(void *where) const final { CallGenerateValueOn(*fSubFields[0], where); }

   std::size_t AppendImpl(const void *from) final { return CallAppendOn(*fSubFields[0], from); }
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final { CallReadOn(*fSubFields[0], globalIndex, to); }

public:
   REnumField(std::string_view fieldName, std::string_view enumName);
   REnumField(REnumField &&other) = default;
   REnumField &operator=(REnumField &&other) = default;
   ~REnumField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const final { return fSubFields[0]->GetValueSize(); }
   size_t GetAlignment() const final { return fSubFields[0]->GetAlignment(); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

/// The field for a class representing a collection of elements via `TVirtualCollectionProxy`.
/// Objects of such type behave as collections that can be accessed through the corresponding member functions in
/// `TVirtualCollectionProxy`. For STL collections, these proxies are provided. Custom classes need to implement the
/// corresponding member functions in `TVirtualCollectionProxy`. At a bare minimum, the user is required to provide an
/// implementation for the following functions in `TVirtualCollectionProxy`: `HasPointers()`, `GetProperties()`,
/// `GetValueClass()`, `GetType()`, `PushProxy()`, `PopProxy()`, `GetFunctionCreateIterators()`, `GetFunctionNext()`,
/// and `GetFunctionDeleteTwoIterators()`.
///
/// The collection proxy for a given class can be set via `TClass::CopyCollectionProxy()`.
class RProxiedCollectionField : public Detail::RFieldBase {
protected:
   /// Allows for iterating over the elements of a proxied collection. RCollectionIterableOnce avoids an additional
   /// iterator copy (see `TVirtualCollectionProxy::GetFunctionCopyIterator`) and thus can only be iterated once.
   class RCollectionIterableOnce {
   public:
      struct RIteratorFuncs {
         TVirtualCollectionProxy::CreateIterators_t fCreateIterators;
         TVirtualCollectionProxy::DeleteTwoIterators_t fDeleteTwoIterators;
         TVirtualCollectionProxy::Next_t fNext;
      };
      static RIteratorFuncs GetIteratorFuncs(TVirtualCollectionProxy *proxy, bool readFromDisk);

   private:
      class RIterator {
         const RCollectionIterableOnce &fOwner;
         void *fIterator = nullptr;
         void *fElementPtr = nullptr;

         void Advance()
         {
            auto fnNext_Contig = [&]() {
               // Array-backed collections (e.g. kSTLvector) directly use the pointer-to-iterator-data as a
               // pointer-to-element, thus saving an indirection level (see documentation for TVirtualCollectionProxy)
               auto &iter = reinterpret_cast<unsigned char *&>(fIterator), p = iter;
               iter += fOwner.fStride;
               return p;
            };
            fElementPtr = fOwner.fStride ? fnNext_Contig() : fOwner.fIFuncs.fNext(fIterator, fOwner.fEnd);
         }

      public:
         using iterator_category = std::forward_iterator_tag;
         using iterator = RIterator;
         using difference_type = std::ptrdiff_t;
         using pointer = void *;

         RIterator(const RCollectionIterableOnce &owner) : fOwner(owner) {}
         RIterator(const RCollectionIterableOnce &owner, void *iter) : fOwner(owner), fIterator(iter) { Advance(); }
         iterator operator++()
         {
            Advance();
            return *this;
         }
         pointer operator*() const { return fElementPtr; }
         bool operator!=(const iterator &rh) const { return fElementPtr != rh.fElementPtr; }
         bool operator==(const iterator &rh) const { return fElementPtr == rh.fElementPtr; }
      };

      const RIteratorFuncs &fIFuncs;
      const std::size_t fStride;
      unsigned char fBeginSmallBuf[TVirtualCollectionProxy::fgIteratorArenaSize];
      unsigned char fEndSmallBuf[TVirtualCollectionProxy::fgIteratorArenaSize];
      void *fBegin = &fBeginSmallBuf;
      void *fEnd = &fEndSmallBuf;
   public:
      /// Construct a `RCollectionIterableOnce` that iterates over `collection`.  If elements are guaranteed to be
      /// contiguous in memory (e.g. a vector), `stride` can be provided for faster iteration, i.e. the address of each
      /// element is known given the base pointer.
      RCollectionIterableOnce(void *collection, const RIteratorFuncs &ifuncs, TVirtualCollectionProxy *proxy,
                              std::size_t stride = 0U)
         : fIFuncs(ifuncs), fStride(stride)
      {
         fIFuncs.fCreateIterators(collection, &fBegin, &fEnd, proxy);
      }
      ~RCollectionIterableOnce() { fIFuncs.fDeleteTwoIterators(fBegin, fEnd); }

      RIterator begin() { return RIterator(*this, fBegin); }
      RIterator end() { return fStride ? RIterator(*this, fEnd) : RIterator(*this); }
   };

   std::unique_ptr<TVirtualCollectionProxy> fProxy;
   Int_t fProperties;
   Int_t fCollectionType;
   /// Two sets of functions to operate on iterators, to be used depending on the access type.  The direction preserves
   /// the meaning from TVirtualCollectionProxy, i.e. read from disk / write to disk, respectively
   RCollectionIterableOnce::RIteratorFuncs fIFuncsRead;
   RCollectionIterableOnce::RIteratorFuncs fIFuncsWrite;
   std::size_t fItemSize;
   ClusterSize_t fNWritten;

   /// Constructor used when the value type of the collection is not known in advance, i.e. in the case of custom
   /// collections.
   RProxiedCollectionField(std::string_view fieldName, std::string_view typeName, TClass *classp);
   /// Constructor used when the value type of the collection is known in advance, e.g. in `RSetField`.
   RProxiedCollectionField(std::string_view fieldName, std::string_view typeName,
                           std::unique_ptr<Detail::RFieldBase> itemField);

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;
   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

   void CommitClusterImpl() final { fNWritten = 0; }

public:
   RProxiedCollectionField(std::string_view fieldName, std::string_view typeName);
   RProxiedCollectionField(RProxiedCollectionField &&other) = default;
   RProxiedCollectionField &operator=(RProxiedCollectionField &&other) = default;
   ~RProxiedCollectionField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const override { return fProxy->Sizeof(); }
   size_t GetAlignment() const override { return alignof(std::max_align_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
   void GetCollectionInfo(NTupleSize_t globalIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const
   {
      fPrincipalColumn->GetCollectionInfo(globalIndex, collectionStart, size);
   }
   void GetCollectionInfo(const RClusterIndex &clusterIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const
   {
      fPrincipalColumn->GetCollectionInfo(clusterIndex, collectionStart, size);
   }
};

/// The field for an untyped record. The subfields are stored consequitively in a memory block, i.e.
/// the memory layout is identical to one that a C++ struct would have
class RRecordField : public Detail::RFieldBase {
protected:
   std::size_t fMaxAlignment = 1;
   std::size_t fSize = 0;
   std::vector<std::size_t> fOffsets;

   std::size_t GetItemPadding(std::size_t baseOffset, std::size_t itemAlignment) const;

   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;

   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor &) final {}

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;
   void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to) final;

   RRecordField(std::string_view fieldName, std::vector<std::unique_ptr<Detail::RFieldBase>> &&itemFields,
                const std::vector<std::size_t> &offsets, std::string_view typeName = "");

   template <std::size_t N>
   RRecordField(std::string_view fieldName, std::array<std::unique_ptr<Detail::RFieldBase>, N> &&itemFields,
                const std::array<std::size_t, N> &offsets, std::string_view typeName = "")
      : ROOT::Experimental::Detail::RFieldBase(fieldName, typeName, ENTupleStructure::kRecord, false /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
      for (unsigned i = 0; i < N; ++i) {
         fOffsets.push_back(offsets[i]);
         fMaxAlignment = std::max(fMaxAlignment, itemFields[i]->GetAlignment());
         fSize += GetItemPadding(fSize, itemFields[i]->GetAlignment()) + itemFields[i]->GetValueSize();
         fTraits &= itemFields[i]->GetTraits();
         Attach(std::move(itemFields[i]));
      }
   }
public:
   /// Construct a RRecordField based on a vector of child fields. The ownership of the child fields is transferred
   /// to the RRecordField instance.
   RRecordField(std::string_view fieldName, std::vector<std::unique_ptr<Detail::RFieldBase>> &&itemFields);
   RRecordField(std::string_view fieldName, std::vector<std::unique_ptr<Detail::RFieldBase>> &itemFields);
   RRecordField(RRecordField&& other) = default;
   RRecordField& operator =(RRecordField&& other) = default;
   ~RRecordField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const final { return fSize; }
   size_t GetAlignment() const final { return fMaxAlignment; }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

/// The generic field for a (nested) std::vector<Type> except for std::vector<bool>
class RVectorField : public Detail::RFieldBase {
private:
   std::size_t fItemSize;
   ClusterSize_t fNWritten;

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;
   void GenerateValue(void *where) const override { new (where) std::vector<char>(); }

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

   void CommitClusterImpl() final { fNWritten = 0; }

public:
   RVectorField(std::string_view fieldName, std::unique_ptr<Detail::RFieldBase> itemField);
   RVectorField(RVectorField&& other) = default;
   RVectorField& operator =(RVectorField&& other) = default;
   ~RVectorField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const override { return sizeof(std::vector<char>); }
   size_t GetAlignment() const final { return std::alignment_of<std::vector<char>>(); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
   void GetCollectionInfo(NTupleSize_t globalIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const {
      fPrincipalColumn->GetCollectionInfo(globalIndex, collectionStart, size);
   }
   void GetCollectionInfo(const RClusterIndex &clusterIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const {
      fPrincipalColumn->GetCollectionInfo(clusterIndex, collectionStart, size);
   }
};

/// The type-erased field for a RVec<Type>
class RRVecField : public Detail::RFieldBase {
private:
   /// Evaluate the constant returned by GetValueSize.
   // (we separate evaluation from the getter to avoid repeating the computation).
   std::size_t EvalValueSize() const;

protected:
   std::size_t fItemSize;
   ClusterSize_t fNWritten;
   std::size_t fValueSize;

   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;
   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   std::size_t AppendImpl(const void *from) override;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) override;
   std::size_t ReadBulkImpl(const RBulkSpec &bulkSpec) final;

   void CommitClusterImpl() final { fNWritten = 0; }

public:
   RRVecField(std::string_view fieldName, std::unique_ptr<Detail::RFieldBase> itemField);
   RRVecField(RRVecField &&) = default;
   RRVecField &operator=(RRVecField &&) = default;
   RRVecField(const RRVecField &) = delete;
   RRVecField &operator=(RRVecField &) = delete;
   ~RRVecField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const override;
   size_t GetAlignment() const override;
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
   void GetCollectionInfo(NTupleSize_t globalIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const
   {
      fPrincipalColumn->GetCollectionInfo(globalIndex, collectionStart, size);
   }
   void GetCollectionInfo(const RClusterIndex &clusterIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const
   {
      fPrincipalColumn->GetCollectionInfo(clusterIndex, collectionStart, size);
   }
};

/// The generic field for fixed size arrays, which do not need an offset column
class RArrayField : public Detail::RFieldBase {
private:
   std::size_t fItemSize;
   std::size_t fArrayLength;

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;

   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor &) final {}

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;
   void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to) final;

public:
   RArrayField(std::string_view fieldName, std::unique_ptr<Detail::RFieldBase> itemField, std::size_t arrayLength);
   RArrayField(RArrayField &&other) = default;
   RArrayField& operator =(RArrayField &&other) = default;
   ~RArrayField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetLength() const { return fArrayLength; }
   size_t GetValueSize() const final { return fItemSize * fArrayLength; }
   size_t GetAlignment() const final { return fSubFields[0]->GetAlignment(); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

/// The generic field an std::bitset<N>. All compilers we care about store the bits in an array of unsigned long.
/// TODO(jblomer): reading and writing efficiency should be improved; currently it is one bit at a time
/// with an array of bools on the page level.
class RBitsetField : public Detail::RFieldBase {
   using Word_t = unsigned long;
   static constexpr std::size_t kWordSize = sizeof(Word_t);
   static constexpr std::size_t kBitsPerWord = kWordSize * 8;

protected:
   std::size_t fN;

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final
   {
      return std::make_unique<RBitsetField>(newName, fN);
   }
   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { memset(where, 0, GetValueSize()); }
   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

public:
   RBitsetField(std::string_view fieldName, std::size_t N);
   RBitsetField(RBitsetField &&other) = default;
   RBitsetField &operator=(RBitsetField &&other) = default;
   ~RBitsetField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return kWordSize * ((fN + kBitsPerWord - 1) / kBitsPerWord); }
   size_t GetAlignment() const final { return alignof(Word_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;

   /// Get the number of bits in the bitset, i.e. the N in std::bitset<N>
   std::size_t GetN() const { return fN; }
};

/// The generic field for std::variant types
class RVariantField : public Detail::RFieldBase {
private:
   size_t fMaxItemSize = 0;
   size_t fMaxAlignment = 1;
   /// In the std::variant memory layout, at which byte number is the index stored
   size_t fTagOffset = 0;
   std::vector<ClusterSize_t::ValueType> fNWritten;

   static std::string GetTypeList(const std::vector<Detail::RFieldBase *> &itemFields);
   /// Extracts the index from an std::variant and transforms it into the 1-based index used for the switch column
   std::uint32_t GetTag(const void *variantPtr) const;
   void SetTag(void *variantPtr, std::uint32_t tag) const;

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

   void CommitClusterImpl() final;

public:
   // TODO(jblomer): use std::span in signature
   RVariantField(std::string_view fieldName, const std::vector<Detail::RFieldBase *> &itemFields);
   RVariantField(RVariantField &&other) = default;
   RVariantField& operator =(RVariantField &&other) = default;
   ~RVariantField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final;
   size_t GetAlignment() const final { return fMaxAlignment; }
};

/// The generic field for a std::set<Type>
class RSetField : public RProxiedCollectionField {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;

public:
   RSetField(std::string_view fieldName, std::string_view typeName, std::unique_ptr<Detail::RFieldBase> itemField);
   RSetField(RSetField &&other) = default;
   RSetField &operator=(RSetField &&other) = default;
   ~RSetField() override = default;

   size_t GetAlignment() const override { return std::alignment_of<std::set<std::max_align_t>>(); }
};

/// The field for values that may or may not be present in an entry. Parent class for unique pointer field and
/// optional field. A nullable field cannot be instantiated itself but only its descendants.
/// The RNullableField takes care of the on-disk representation. Child classes are responsible for the in-memory
/// representation.  The on-disk representation can be "dense" or "sparse". Dense nullable fields have a bitmask
/// (true: item available, false: item missing) and serialize a default-constructed item for missing items.
/// Sparse nullable fields use a (Split)Index[64|32] column to point to the available items.
/// By default, items whose size is smaller or equal to 4 bytes (size of (Split)Index32 column element) are stored
/// densely.
class RNullableField : public Detail::RFieldBase {
   /// For a dense nullable field, used to write a default-constructed item for missing ones.
   std::unique_ptr<RValue> fDefaultItemValue;
   /// For a sparse nullable field, the number of written non-null items in this cluster
   ClusterSize_t fNWritten{0};

protected:
   const Detail::RFieldBase::RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &) final;

   std::size_t AppendNull();
   std::size_t AppendValue(const void *from);
   void CommitClusterImpl() final { fNWritten = 0; }

   /// Given the index of the nullable field, returns the corresponding global index of the subfield or,
   /// if it is null, returns kInvalidClusterIndex
   RClusterIndex GetItemIndex(NTupleSize_t globalIndex);

   RNullableField(std::string_view fieldName, std::string_view typeName, std::unique_ptr<Detail::RFieldBase> itemField);

public:
   RNullableField(RNullableField &&other) = default;
   RNullableField &operator=(RNullableField &&other) = default;
   ~RNullableField() override = default;

   bool IsDense() const { return GetColumnRepresentative()[0] ==  EColumnType::kBit; }
   bool IsSparse() const { return !IsDense(); }
   void SetDense() { SetColumnRepresentative({EColumnType::kBit}); }
   void SetSparse() { SetColumnRepresentative({EColumnType::kSplitIndex32}); }

   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

class RUniquePtrField : public RNullableField {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;

   void GenerateValue(void *where) const final { new (where) std::unique_ptr<char>(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

public:
   RUniquePtrField(std::string_view fieldName, std::string_view typeName,
                   std::unique_ptr<Detail::RFieldBase> itemField);
   RUniquePtrField(RUniquePtrField &&other) = default;
   RUniquePtrField &operator=(RUniquePtrField &&other) = default;
   ~RUniquePtrField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;
   size_t GetValueSize() const final { return sizeof(std::unique_ptr<char>); }
   size_t GetAlignment() const final { return alignof(std::unique_ptr<char>); }
};

class RAtomicField : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;
   void GenerateColumnsImpl() final {}
   void GenerateColumnsImpl(const RNTupleDescriptor &) final {}

   void GenerateValue(void *where) const final { CallGenerateValueOn(*fSubFields[0], where); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final
   {
      CallDestroyValueOn(*fSubFields[0], objPtr, dtorOnly);
   }

   std::size_t AppendImpl(const void *from) final { return CallAppendOn(*fSubFields[0], from); }
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final { CallReadOn(*fSubFields[0], globalIndex, to); }
   void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to) final
   {
      CallReadOn(*fSubFields[0], clusterIndex, to);
   }

public:
   RAtomicField(std::string_view fieldName, std::string_view typeName, std::unique_ptr<Detail::RFieldBase> itemField);
   RAtomicField(RAtomicField &&other) = default;
   RAtomicField &operator=(RAtomicField &&other) = default;
   ~RAtomicField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;

   size_t GetValueSize() const final { return fSubFields[0]->GetValueSize(); }
   size_t GetAlignment() const final { return fSubFields[0]->GetAlignment(); }

   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

/// Classes with dictionaries that can be inspected by TClass
template <typename T, typename=void>
class RField : public RClassField {
protected:
   void GenerateValue(void *where) const final
   {
      if constexpr (std::is_default_constructible_v<T>) {
         new (where) T();
      } else {
         // If there is no default constructor, try with the IO constructor
         new (where) T(static_cast<TRootIOCtor *>(nullptr));
      }
   }

public:
   static std::string TypeName() { return ROOT::Internal::GetDemangledTypeName(typeid(T)); }
   RField(std::string_view name) : RClassField(name, TypeName()) {
      static_assert(std::is_class_v<T>, "no I/O support for this basic C++ type");
   }
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename T>
class RField<T, typename std::enable_if<std::is_enum_v<T>>::type> : public REnumField {
public:
   static std::string TypeName() { return ROOT::Internal::GetDemangledTypeName(typeid(T)); }
   RField(std::string_view name) : REnumField(name, TypeName()) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;
};

template <typename T, typename = void>
struct HasCollectionProxyMemberType : std::false_type {
};
template <typename T>
struct HasCollectionProxyMemberType<
   T, typename std::enable_if<std::is_same<typename T::IsCollectionProxy, std::true_type>::value>::type>
   : std::true_type {
};

/// The point here is that we can only tell at run time if a class has an associated collection proxy.
/// For compile time, in the first iteration of this PR we had an extra template argument that acted as a "tag" to
/// differentiate the RField specialization for classes with an associated collection proxy (inherits
/// `RProxiedCollectionField`) from the RField primary template definition (`RClassField`-derived), as in:
/// ```
/// auto field = std::make_unique<RField<MyClass>>("klass");
/// // vs
/// auto otherField = std::make_unique<RField<MyClass, ROOT::Experimental::TagIsCollectionProxy>>("klass");
/// ```
///
/// That is convenient only for non-nested types, i.e. it doesn't work with, e.g. `RField<std::vector<MyClass>,
/// ROOT::Experimental::TagIsCollectionProxy>`, as the tag is not forwarded to the instantiation of the inner RField
/// (that for the value type of the vector).  The following two possible solutions were considered:
/// - A wrapper type (much like `ntuple/v7/inc/ROOT/RNTupleUtil.hxx:49`), that helps to differentiate both cases.
/// There we would have:
/// ```
/// auto field = std::make_unique<RField<RProxiedCollection<MyClass>>>("klass"); // Using collection proxy
/// ```
/// - A helper `IsCollectionProxy<T>` type, that can be used in a similar way to those in the `<type_traits>` header.
/// We found this more convenient and is the implemented thing below.  Here, classes can be marked as a
/// collection proxy with either of the following two forms (whichever is more convenient for the user):
/// ```
/// template <>
/// struct IsCollectionProxy<MyClass> : std::true_type {};
/// ```
/// or by adding a member type to the class as follows:
/// ```
/// class MyClass {
/// public:
///    using IsCollectionProxy = std::true_type;
/// };
/// ```
///
/// Of course, there is another possible solution which is to have a single `RClassField` that implements both
/// the regular-class and the collection-proxy behaviors, and always chooses appropriately at run time.
/// We found that less clean and probably has more overhead, as most probably it involves an additional branch + call
/// in each of the member functions.
template <typename T, typename = void>
struct IsCollectionProxy : HasCollectionProxyMemberType<T> {
};

/// Classes behaving as a collection of elements that can be queried via the `TVirtualCollectionProxy` interface
/// The use of a collection proxy for a particular class can be enabled via:
/// ```
/// namespace ROOT::Experimental {
///    template <> struct IsCollectionProxy<Classname> : std::true_type {};
/// }
/// ```
/// Alternatively, this can be achieved by adding a member type to the class definition as follows:
/// ```
/// class Classname {
/// public:
///    using IsCollectionProxy = std::true_type;
/// };
/// ```
template <typename T>
class RField<T, typename std::enable_if<IsCollectionProxy<T>::value>::type> : public RProxiedCollectionField {
protected:
   void GenerateValue(void *where) const final { new (where) T(); }

public:
   static std::string TypeName() { return ROOT::Internal::GetDemangledTypeName(typeid(T)); }
   RField(std::string_view name) : RProxiedCollectionField(name, TypeName())
   {
      static_assert(std::is_class<T>::value, "collection proxy unsupported for fundamental types");
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

/// The collection field is only used for writing; when reading, untyped collections are projected to an std::vector
class RCollectionField : public ROOT::Experimental::Detail::RFieldBase {
private:
   /// Save the link to the collection ntuple in order to reset the offset counter when committing the cluster
   std::shared_ptr<RCollectionNTupleWriter> fCollectionNTuple;

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final;
   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *) const final {}

   void CommitClusterImpl() final;

public:
   static std::string TypeName() { return ""; }
   RCollectionField(std::string_view name,
                    std::shared_ptr<RCollectionNTupleWriter> collectionNTuple,
                    std::unique_ptr<RNTupleModel> collectionModel);
   RCollectionField(RCollectionField&& other) = default;
   RCollectionField& operator =(RCollectionField&& other) = default;
   ~RCollectionField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(ClusterSize_t); }
   size_t GetAlignment() const final { return alignof(ClusterSize_t); }
};

/// The generic field for `std::pair<T1, T2>` types
class RPairField : public RRecordField {
private:
   TClass *fClass = nullptr;
   static std::string GetTypeList(const std::array<std::unique_ptr<Detail::RFieldBase>, 2> &itemFields);

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   RPairField(std::string_view fieldName, std::array<std::unique_ptr<Detail::RFieldBase>, 2> &&itemFields,
              const std::array<std::size_t, 2> &offsets);

public:
   RPairField(std::string_view fieldName, std::array<std::unique_ptr<Detail::RFieldBase>, 2> &itemFields);
   RPairField(RPairField &&other) = default;
   RPairField &operator=(RPairField &&other) = default;
   ~RPairField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

/// The generic field for `std::tuple<Ts...>` types
class RTupleField : public RRecordField {
private:
   TClass *fClass = nullptr;
   static std::string GetTypeList(const std::vector<std::unique_ptr<Detail::RFieldBase>> &itemFields);

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const override;

   void GenerateValue(void *where) const override;
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   RTupleField(std::string_view fieldName, std::vector<std::unique_ptr<Detail::RFieldBase>> &&itemFields,
               const std::vector<std::size_t> &offsets);

public:
   RTupleField(std::string_view fieldName, std::vector<std::unique_ptr<Detail::RFieldBase>> &itemFields);
   RTupleField(RTupleField &&other) = default;
   RTupleField &operator=(RTupleField &&other) = default;
   ~RTupleField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

/// An artificial field that transforms an RNTuple column that contains the offset of collections into
/// collection sizes. It is only used for reading, e.g. as projected field or as an artificial field that provides the
/// "number of" RDF columns for collections (e.g. `R_rdf_sizeof_jets` for a collection named `jets`).
/// It is used in the templated RField<RNTupleCardinality<SizeT>> form, which represents the collection sizes either
/// as 32bit unsigned int (std::uint32_t) or as 64bit unsigned int (std::uint64_t).
class RCardinalityField : public Detail::RFieldBase {
protected:
   RCardinalityField(std::string_view fieldName, std::string_view typeName)
      : Detail::RFieldBase(fieldName, typeName, ENTupleStructure::kLeaf, false /* isSimple */)
   {
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   // Field is only used for reading
   void GenerateColumnsImpl() final { throw RException(R__FAIL("Cardinality fields must only be used for reading")); }
   void GenerateColumnsImpl(const RNTupleDescriptor &) final;

public:
   RCardinalityField(RCardinalityField &&other) = default;
   RCardinalityField &operator=(RCardinalityField &&other) = default;
   ~RCardinalityField() = default;

   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;

   const RField<RNTupleCardinality<std::uint32_t>> *As32Bit() const;
   const RField<RNTupleCardinality<std::uint64_t>> *As64Bit() const;
};

////////////////////////////////////////////////////////////////////////////////
/// Template specializations for concrete C++ types
////////////////////////////////////////////////////////////////////////////////

template <>
class RField<ClusterSize_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) ClusterSize_t(0); }

public:
   static std::string TypeName() { return "ROOT::Experimental::ClusterSize_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   ClusterSize_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<ClusterSize_t>(globalIndex);
   }
   ClusterSize_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<ClusterSize_t>(clusterIndex);
   }
   ClusterSize_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<ClusterSize_t>(globalIndex, nItems);
   }
   ClusterSize_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<ClusterSize_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(ClusterSize_t); }
   size_t GetAlignment() const final { return alignof(ClusterSize_t); }

   /// Special help for offset fields
   void GetCollectionInfo(NTupleSize_t globalIndex, RClusterIndex *collectionStart, ClusterSize_t *size) {
      fPrincipalColumn->GetCollectionInfo(globalIndex, collectionStart, size);
   }
   void GetCollectionInfo(const RClusterIndex &clusterIndex, RClusterIndex *collectionStart, ClusterSize_t *size) {
      fPrincipalColumn->GetCollectionInfo(clusterIndex, collectionStart, size);
   }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <typename SizeT>
class RField<RNTupleCardinality<SizeT>> : public RCardinalityField {
protected:
   std::unique_ptr<ROOT::Experimental::Detail::RFieldBase> CloneImpl(std::string_view newName) const final
   {
      return std::make_unique<RField<RNTupleCardinality<SizeT>>>(newName);
   }
   void GenerateValue(void *where) const final { new (where) RNTupleCardinality<SizeT>(0); }

public:
   static std::string TypeName() { return "ROOT::Experimental::RNTupleCardinality<" + RField<SizeT>::TypeName() + ">"; }
   explicit RField(std::string_view name) : RCardinalityField(name, TypeName()) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(RNTupleCardinality<SizeT>); }
   size_t GetAlignment() const final { return alignof(RNTupleCardinality<SizeT>); }

   /// Get the number of elements of the collection identified by globalIndex
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final
   {
      RClusterIndex collectionStart;
      ClusterSize_t size;
      fPrincipalColumn->GetCollectionInfo(globalIndex, &collectionStart, &size);
      *static_cast<RNTupleCardinality<SizeT> *>(to) = size;
   }

   /// Get the number of elements of the collection identified by clusterIndex
   void ReadInClusterImpl(const RClusterIndex &clusterIndex, void *to) final
   {
      RClusterIndex collectionStart;
      ClusterSize_t size;
      fPrincipalColumn->GetCollectionInfo(clusterIndex, &collectionStart, &size);
      *static_cast<RNTupleCardinality<SizeT> *>(to) = size;
   }

   std::size_t ReadBulkImpl(const RBulkSpec &bulkSpec) final
   {
      RClusterIndex collectionStart;
      ClusterSize_t collectionSize;
      fPrincipalColumn->GetCollectionInfo(bulkSpec.fFirstIndex, &collectionStart, &collectionSize);

      auto typedValues = static_cast<RNTupleCardinality<SizeT> *>(bulkSpec.fValues);
      typedValues[0] = collectionSize;

      auto lastOffset = collectionStart.GetIndex() + collectionSize;
      ClusterSize_t::ValueType nRemainingEntries = bulkSpec.fCount - 1;
      std::size_t nEntries = 1;
      while (nRemainingEntries > 0) {
         NTupleSize_t nItemsUntilPageEnd;
         auto offsets = fPrincipalColumn->MapV<ClusterSize_t>(bulkSpec.fFirstIndex + nEntries, nItemsUntilPageEnd);
         std::size_t nBatch = std::min(nRemainingEntries, nItemsUntilPageEnd);
         for (std::size_t i = 0; i < nBatch; ++i) {
            typedValues[nEntries + i] = offsets[i] - lastOffset;
            lastOffset = offsets[i];
         }
         nRemainingEntries -= nBatch;
         nEntries += nBatch;
      }
      return RBulkSpec::kAllSet;
   }
};

template <>
class RField<bool> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) bool(false); }

public:
   static std::string TypeName() { return "bool"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   bool *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<bool>(globalIndex);
   }
   bool *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<bool>(clusterIndex);
   }
   bool *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<bool>(globalIndex, nItems);
   }
   bool *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<bool>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(bool); }
   size_t GetAlignment() const final { return alignof(bool); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<float> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) float(0.0); }

public:
   static std::string TypeName() { return "float"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   float *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<float>(globalIndex);
   }
   float *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<float>(clusterIndex);
   }
   float *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<float>(globalIndex, nItems);
   }
   float *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<float>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(float); }
   size_t GetAlignment() const final { return alignof(float); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};


template <>
class RField<double> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) double(0.0); }

public:
   static std::string TypeName() { return "double"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   double *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<double>(globalIndex);
   }
   double *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<double>(clusterIndex);
   }
   double *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<double>(globalIndex, nItems);
   }
   double *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<double>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(double); }
   size_t GetAlignment() const final { return alignof(double); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;

   // Set the column representation to 32 bit floating point and the type alias to Double32_t
   void SetDouble32();
};

template <>
class RField<std::byte> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final
   {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) std::byte{0}; }

public:
   static std::string TypeName() { return "std::byte"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   std::byte *Map(NTupleSize_t globalIndex) { return fPrincipalColumn->Map<std::byte>(globalIndex); }
   std::byte *Map(const RClusterIndex &clusterIndex) { return fPrincipalColumn->Map<std::byte>(clusterIndex); }
   std::byte *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems)
   {
      return fPrincipalColumn->MapV<std::byte>(globalIndex, nItems);
   }
   std::byte *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems)
   {
      return fPrincipalColumn->MapV<std::byte>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::byte); }
   size_t GetAlignment() const final { return alignof(std::byte); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<char> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) char(0); }

public:
   static std::string TypeName() { return "char"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   char *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<char>(globalIndex);
   }
   char *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<char>(clusterIndex);
   }
   char *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<char>(globalIndex, nItems);
   }
   char *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<char>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(char); }
   size_t GetAlignment() const final { return alignof(char); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::int8_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) int8_t(0); }

public:
   static std::string TypeName() { return "std::int8_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::int8_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::int8_t>(globalIndex);
   }
   std::int8_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::int8_t>(clusterIndex);
   }
   std::int8_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int8_t>(globalIndex, nItems);
   }
   std::int8_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int8_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::int8_t); }
   size_t GetAlignment() const final { return alignof(std::int8_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::uint8_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) uint8_t(0); }

public:
   static std::string TypeName() { return "std::uint8_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::uint8_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::uint8_t>(globalIndex);
   }
   std::uint8_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::uint8_t>(clusterIndex);
   }
   std::uint8_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint8_t>(globalIndex, nItems);
   }
   std::uint8_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint8_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::uint8_t); }
   size_t GetAlignment() const final { return alignof(std::uint8_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::int16_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) int16_t(0); }

public:
   static std::string TypeName() { return "std::int16_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::int16_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::int16_t>(globalIndex);
   }
   std::int16_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::int16_t>(clusterIndex);
   }
   std::int16_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int16_t>(globalIndex, nItems);
   }
   std::int16_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int16_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::int16_t); }
   size_t GetAlignment() const final { return alignof(std::int16_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::uint16_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) int16_t(0); }

public:
   static std::string TypeName() { return "std::uint16_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::uint16_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::uint16_t>(globalIndex);
   }
   std::uint16_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::uint16_t>(clusterIndex);
   }
   std::uint16_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint16_t>(globalIndex, nItems);
   }
   std::uint16_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint16_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::uint16_t); }
   size_t GetAlignment() const final { return alignof(std::uint16_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::int32_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) int32_t(0); }

public:
   static std::string TypeName() { return "std::int32_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::int32_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::int32_t>(globalIndex);
   }
   std::int32_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::int32_t>(clusterIndex);
   }
   std::int32_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int32_t>(globalIndex, nItems);
   }
   std::int32_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int32_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::int32_t); }
   size_t GetAlignment() const final { return alignof(std::int32_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::uint32_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) uint32_t(0); }

public:
   static std::string TypeName() { return "std::uint32_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::uint32_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::uint32_t>(globalIndex);
   }
   std::uint32_t *Map(const RClusterIndex clusterIndex) {
      return fPrincipalColumn->Map<std::uint32_t>(clusterIndex);
   }
   std::uint32_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint32_t>(globalIndex, nItems);
   }
   std::uint32_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint32_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::uint32_t); }
   size_t GetAlignment() const final { return alignof(std::uint32_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::uint64_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) uint64_t(0); }

public:
   static std::string TypeName() { return "std::uint64_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::uint64_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::uint64_t>(globalIndex);
   }
   std::uint64_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::uint64_t>(clusterIndex);
   }
   std::uint64_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint64_t>(globalIndex, nItems);
   }
   std::uint64_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::uint64_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::uint64_t); }
   size_t GetAlignment() const final { return alignof(std::uint64_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::int64_t> : public Detail::RFieldBase {
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;
   void GenerateValue(void *where) const final { new (where) int64_t(0); }

public:
   static std::string TypeName() { return "std::int64_t"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, true /* isSimple */)
   {
      fTraits |= kTraitTrivialType;
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   std::int64_t *Map(NTupleSize_t globalIndex) {
      return fPrincipalColumn->Map<std::int64_t>(globalIndex);
   }
   std::int64_t *Map(const RClusterIndex &clusterIndex) {
      return fPrincipalColumn->Map<std::int64_t>(clusterIndex);
   }
   std::int64_t *MapV(NTupleSize_t globalIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int64_t>(globalIndex, nItems);
   }
   std::int64_t *MapV(const RClusterIndex &clusterIndex, NTupleSize_t &nItems) {
      return fPrincipalColumn->MapV<std::int64_t>(clusterIndex, nItems);
   }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::int64_t); }
   size_t GetAlignment() const final { return alignof(std::int64_t); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};

template <>
class RField<std::string> : public Detail::RFieldBase {
private:
   ClusterSize_t fIndex;

   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void GenerateValue(void *where) const final { new (where) std::string(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const override;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(ROOT::Experimental::NTupleSize_t globalIndex, void *to) final;

   void CommitClusterImpl() final { fIndex = 0; }

public:
   static std::string TypeName() { return "std::string"; }
   explicit RField(std::string_view name)
      : Detail::RFieldBase(name, TypeName(), ENTupleStructure::kLeaf, false /* isSimple */), fIndex(0)
   {
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(std::string); }
   size_t GetAlignment() const final { return std::alignment_of<std::string>(); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
};


template <typename ItemT, std::size_t N>
class RField<std::array<ItemT, N>> : public RArrayField {
   using ContainerT = typename std::array<ItemT, N>;

protected:
   void GenerateValue(void *where) const final { new (where) ContainerT(); }

public:
   static std::string TypeName() {
      return "std::array<" + RField<ItemT>::TypeName() + "," + std::to_string(N) + ">";
   }
   explicit RField(std::string_view name) : RArrayField(name, std::make_unique<RField<ItemT>>("_0"), N)
   {}
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename ItemT, std::size_t N>
class RField<ItemT[N]> : public RField<std::array<ItemT, N>> {
public:
   explicit RField(std::string_view name) : RField<std::array<ItemT, N>>(name) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;
};

template <typename ItemT>
class RField<std::set<ItemT>> : public RSetField {
   using ContainerT = typename std::set<ItemT>;

protected:
   void GenerateValue(void *where) const final { new (where) ContainerT(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final
   {
      std::destroy_at(static_cast<ContainerT *>(objPtr));
      Detail::RFieldBase::DestroyValue(objPtr, dtorOnly);
   }

public:
   static std::string TypeName() { return "std::set<" + RField<ItemT>::TypeName() + ">"; }

   explicit RField(std::string_view name) : RSetField(name, TypeName(), std::make_unique<RField<ItemT>>("_0")) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(ContainerT); }
   size_t GetAlignment() const final { return std::alignment_of<ContainerT>(); }
};

template <typename... ItemTs>
class RField<std::variant<ItemTs...>> : public RVariantField {
   using ContainerT = typename std::variant<ItemTs...>;
private:
   template <typename HeadT, typename... TailTs>
   static std::string BuildItemTypes()
   {
      std::string result = RField<HeadT>::TypeName();
      if constexpr(sizeof...(TailTs) > 0)
         result += "," + BuildItemTypes<TailTs...>();
      return result;
   }

   template <typename HeadT, typename... TailTs>
   static std::vector<Detail::RFieldBase *> BuildItemFields(unsigned int index = 0)
   {
      std::vector<Detail::RFieldBase *> result;
      result.emplace_back(new RField<HeadT>("_" + std::to_string(index)));
      if constexpr(sizeof...(TailTs) > 0) {
         auto tailFields = BuildItemFields<TailTs...>(index + 1);
         result.insert(result.end(), tailFields.begin(), tailFields.end());
      }
      return result;
   }

protected:
   void GenerateValue(void *where) const final { new (where) ContainerT(); }

public:
   static std::string TypeName() { return "std::variant<" + BuildItemTypes<ItemTs...>() + ">"; }
   explicit RField(std::string_view name) : RVariantField(name, BuildItemFields<ItemTs...>()) {}
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename ItemT>
class RField<std::vector<ItemT>> : public RVectorField {
   using ContainerT = typename std::vector<ItemT>;

protected:
   void GenerateValue(void *where) const final { new (where) ContainerT(); }

public:
   static std::string TypeName() { return "std::vector<" + RField<ItemT>::TypeName() + ">"; }
   explicit RField(std::string_view name)
      : RVectorField(name, std::make_unique<RField<ItemT>>("_0"))
   {}
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(ContainerT); }
};

// std::vector<bool> is a template specialization and needs special treatment
template <>
class RField<std::vector<bool>> : public Detail::RFieldBase {
private:
   ClusterSize_t fNWritten{0};

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      return std::make_unique<RField>(newName);
   }

   const RColumnRepresentations &GetColumnRepresentations() const final;
   void GenerateColumnsImpl() final;
   void GenerateColumnsImpl(const RNTupleDescriptor &desc) final;

   void GenerateValue(void *where) const final { new (where) std::vector<bool>(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final;

   std::size_t AppendImpl(const void *from) final;
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final;

   void CommitClusterImpl() final { fNWritten = 0; }

public:
   static std::string TypeName() { return "std::vector<bool>"; }
   explicit RField(std::string_view name);
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
   std::vector<RValue> SplitValue(const RValue &value) const final;

   size_t GetValueSize() const final { return sizeof(std::vector<bool>); }
   size_t GetAlignment() const final { return std::alignment_of<std::vector<bool>>(); }
   void AcceptVisitor(Detail::RFieldVisitor &visitor) const final;
   void GetCollectionInfo(NTupleSize_t globalIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const {
      fPrincipalColumn->GetCollectionInfo(globalIndex, collectionStart, size);
   }
   void GetCollectionInfo(const RClusterIndex &clusterIndex, RClusterIndex *collectionStart, ClusterSize_t *size) const
   {
      fPrincipalColumn->GetCollectionInfo(clusterIndex, collectionStart, size);
   }
};

template <typename ItemT>
class RField<ROOT::VecOps::RVec<ItemT>> : public RRVecField {
   using ContainerT = typename ROOT::VecOps::RVec<ItemT>;
protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final {
      auto newItemField = fSubFields[0]->Clone(fSubFields[0]->GetName());
      return std::make_unique<RField<ROOT::VecOps::RVec<ItemT>>>(newName, std::move(newItemField));
   }

   void GenerateValue(void *where) const final { new (where) ContainerT(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final
   {
      std::destroy_at(static_cast<ContainerT *>(objPtr));
      Detail::RFieldBase::DestroyValue(objPtr, dtorOnly);
   }

   std::size_t AppendImpl(const void *from) final
   {
      auto typedValue = static_cast<const ContainerT *>(from);
      auto nbytes = 0;
      auto count = typedValue->size();
      for (unsigned i = 0; i < count; ++i) {
         nbytes += CallAppendOn(*fSubFields[0], &typedValue->data()[i]);
      }
      this->fNWritten += count;
      fColumns[0]->Append(&this->fNWritten);
      return nbytes + fColumns[0]->GetElement()->GetPackedSize();
   }
   void ReadGlobalImpl(NTupleSize_t globalIndex, void *to) final
   {
      auto typedValue = static_cast<ContainerT *>(to);
      ClusterSize_t nItems;
      RClusterIndex collectionStart;
      fPrincipalColumn->GetCollectionInfo(globalIndex, &collectionStart, &nItems);
      typedValue->resize(nItems);
      for (unsigned i = 0; i < nItems; ++i) {
         CallReadOn(*fSubFields[0], collectionStart + i, &typedValue->data()[i]);
      }
   }

public:
   RField(std::string_view fieldName, std::unique_ptr<Detail::RFieldBase> itemField)
      : RRVecField(fieldName, std::move(itemField))
   {
   }

   explicit RField(std::string_view name)
      : RField(name, std::make_unique<RField<ItemT>>("_0"))
   {
   }
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   static std::string TypeName() { return "ROOT::VecOps::RVec<" + RField<ItemT>::TypeName() + ">"; }

   using Detail::RFieldBase::GenerateValue;
   size_t GetValueSize() const final { return sizeof(ContainerT); }
   size_t GetAlignment() const final { return std::alignment_of<ContainerT>(); }
};

template <typename T1, typename T2>
class RField<std::pair<T1, T2>> : public RPairField {
   using ContainerT = typename std::pair<T1,T2>;
private:
   template <typename Ty1, typename Ty2>
   static std::array<std::unique_ptr<Detail::RFieldBase>, 2> BuildItemFields()
   {
      return {std::make_unique<RField<Ty1>>("_0"), std::make_unique<RField<Ty2>>("_1")};
   }

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final
   {
      std::array<std::unique_ptr<Detail::RFieldBase>, 2> items{fSubFields[0]->Clone(fSubFields[0]->GetName()),
                                                               fSubFields[1]->Clone(fSubFields[1]->GetName())};
      return std::make_unique<RField<std::pair<T1, T2>>>(newName, std::move(items));
   }

   void GenerateValue(void *where) const final { new (where) ContainerT(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final
   {
      std::destroy_at(static_cast<ContainerT *>(objPtr));
      Detail::RFieldBase::DestroyValue(objPtr, dtorOnly);
   }

public:
   static std::string TypeName() {
      return "std::pair<" + RField<T1>::TypeName() + "," + RField<T2>::TypeName() + ">";
   }
   explicit RField(std::string_view name, std::array<std::unique_ptr<Detail::RFieldBase>, 2> &&itemFields)
      : RPairField(name, std::move(itemFields), {offsetof(ContainerT, first), offsetof(ContainerT, second)})
   {
      fMaxAlignment = std::max(alignof(T1), alignof(T2));
      fSize = sizeof(ContainerT);
   }
   explicit RField(std::string_view name) : RField(name, BuildItemFields<T1, T2>()) {}
   RField(RField&& other) = default;
   RField& operator =(RField&& other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename... ItemTs>
class RField<std::tuple<ItemTs...>> : public RTupleField {
   using ContainerT = typename std::tuple<ItemTs...>;
private:
   template <typename HeadT, typename... TailTs>
   static std::string BuildItemTypes()
   {
      std::string result = RField<HeadT>::TypeName();
      if constexpr (sizeof...(TailTs) > 0)
         result += "," + BuildItemTypes<TailTs...>();
      return result;
   }

   template <typename HeadT, typename... TailTs>
   static void _BuildItemFields(std::vector<std::unique_ptr<Detail::RFieldBase>> &itemFields, unsigned int index = 0)
   {
      itemFields.emplace_back(new RField<HeadT>("_" + std::to_string(index)));
      if constexpr (sizeof...(TailTs) > 0)
         _BuildItemFields<TailTs...>(itemFields, index + 1);
   }
   template <typename... Ts>
   static std::vector<std::unique_ptr<Detail::RFieldBase>> BuildItemFields()
   {
      std::vector<std::unique_ptr<Detail::RFieldBase>> result;
      _BuildItemFields<Ts...>(result);
      return result;
   }

   template <unsigned Index, typename HeadT, typename... TailTs>
   static void _BuildItemOffsets(std::vector<std::size_t> &offsets, const ContainerT &tuple)
   {
      auto offset =
         reinterpret_cast<std::uintptr_t>(&std::get<Index>(tuple)) - reinterpret_cast<std::uintptr_t>(&tuple);
      offsets.emplace_back(offset);
      if constexpr (sizeof...(TailTs) > 0)
         _BuildItemOffsets<Index + 1, TailTs...>(offsets, tuple);
   }
   template <typename... Ts>
   static std::vector<std::size_t> BuildItemOffsets()
   {
      std::vector<std::size_t> result;
      _BuildItemOffsets<0, Ts...>(result, ContainerT());
      return result;
   }

protected:
   std::unique_ptr<Detail::RFieldBase> CloneImpl(std::string_view newName) const final
   {
      std::vector<std::unique_ptr<Detail::RFieldBase>> items;
      for (auto &item : fSubFields)
         items.push_back(item->Clone(item->GetName()));
      return std::make_unique<RField<std::tuple<ItemTs...>>>(newName, std::move(items));
   }

   void GenerateValue(void *where) const final { new (where) ContainerT(); }
   void DestroyValue(void *objPtr, bool dtorOnly = false) const final
   {
      std::destroy_at(static_cast<ContainerT *>(objPtr));
      Detail::RFieldBase::DestroyValue(objPtr, dtorOnly);
   }

public:
   static std::string TypeName() { return "std::tuple<" + BuildItemTypes<ItemTs...>() + ">"; }
   explicit RField(std::string_view name, std::vector<std::unique_ptr<Detail::RFieldBase>> &&itemFields)
      : RTupleField(name, std::move(itemFields), BuildItemOffsets<ItemTs...>())
   {
      fMaxAlignment = std::max({alignof(ItemTs)...});
      fSize = sizeof(ContainerT);
   }
   explicit RField(std::string_view name) : RField(name, BuildItemFields<ItemTs...>()) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <std::size_t N>
class RField<std::bitset<N>> : public RBitsetField {
public:
   static std::string TypeName() { return "std::bitset<" + std::to_string(N) + ">"; }
   explicit RField(std::string_view name) : RBitsetField(name, N) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename ItemT>
class RField<std::unique_ptr<ItemT>> : public RUniquePtrField {
public:
   static std::string TypeName() { return "std::unique_ptr<" + RField<ItemT>::TypeName() + ">"; }
   explicit RField(std::string_view name) : RUniquePtrField(name, TypeName(), std::make_unique<RField<ItemT>>("_0")) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

template <typename ItemT>
class RField<std::atomic<ItemT>> : public RAtomicField {
public:
   static std::string TypeName() { return "std::atomic<" + RField<ItemT>::TypeName() + ">"; }
   explicit RField(std::string_view name) : RAtomicField(name, TypeName(), std::make_unique<RField<ItemT>>("_0")) {}
   RField(RField &&other) = default;
   RField &operator=(RField &&other) = default;
   ~RField() override = default;

   using Detail::RFieldBase::GenerateValue;
};

} // namespace Experimental
} // namespace ROOT

#endif
