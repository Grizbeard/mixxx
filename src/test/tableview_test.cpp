// Tests for tableview-related things
// Right now it's just testing the serialize-unserialize of the header state code.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QStringList>
#include <QtDebug>
#include "proto/headers.pb.h"
#include "widget/wtracktableviewheader.h"

class HeaderViewStateTest : public testing::Test {
};

TEST_F(HeaderViewStateTest, RoundTrip) {
    mixxx::library::HeaderViewState headerViewState_pb;
    mixxx::library::HeaderViewState::HeaderState* header_state_pb =
                headerViewState_pb.add_header_state();

    header_state_pb->set_hidden(true);
    header_state_pb->set_size(50);
    header_state_pb->set_logical_index(10);
    header_state_pb->set_visual_index(2);
    header_state_pb->set_column_name("MyCol");

    header_state_pb =
                headerViewState_pb.add_header_state();

    header_state_pb->set_hidden(false);
    header_state_pb->set_size(22);
    header_state_pb->set_logical_index(6);
    header_state_pb->set_visual_index(3);
    header_state_pb->set_column_name("MyOtherCol");

    headerViewState_pb.set_sort_indicator_shown(true);
    headerViewState_pb.set_sort_indicator_section(1);
    headerViewState_pb.set_sort_order(Qt::DescendingOrder);

    // Create a HeaderViewState based on the proto.
    HeaderViewState view_state(headerViewState_pb);

    // Get a serialized form of the state.
    QString saved_state = view_state.saveState();

    // Initialize a new state object with the saved state.
    HeaderViewState loaded_state(saved_state);

    // Compare the old saved state with the new one.
    ASSERT_EQ(saved_state, loaded_state.saveState());

    // Ensure that the serialization is not bullshit.
    ASSERT_NE("", saved_state);
}

TEST_F(HeaderViewStateTest, GoodHeaderState) {
    const QString kGoodSerializedProto("ChEIARAAGAEgACoHcHJldmlldwoICAAQSxgEIAE"
            "KEggBEAAYAiACKghjb3ZlcmFydAoICAEQABgDIAMKCAgBEAAYBSAECggIABBaGBYgB"
            "QoICAEQABgGIAYKCQgAEPwBGBcgBwoJCAAQvAEYByAICgkIABDmARgIIAkKCAgBEAA"
            "YCSAKCggIARAAGAogCwoJCAAQkwQYGSAMCggIARAAGAwgDQoICAEQABgNIA4KCAgBE"
            "AAYDiAPCggIARAAGBAgEAoICAAQVxgRIBEKCAgBEAAYEiASCggIABBGGBMgEwoICAA"
            "QMhgPIBQKCAgAEHUYCyAVCggIARAAGBQgFgoICAAQNxgVIBcKCAgBEAAYGCAYCggIA"
            "RAAGBogGQoICAEQABgbIBoKCAgBEAAYHCAbCggIARAAGAAgHAoICAEQABgdIB0KCAg"
            "BEAAYHiAeEAEYFiAB");

    HeaderViewState view_state(kGoodSerializedProto);
    ASSERT_TRUE(view_state.healthy());
    ASSERT_EQ(kGoodSerializedProto, view_state.saveState());
}

TEST_F(HeaderViewStateTest, BadHeaderState) {
    HeaderViewState view_state("BLAHBLAHBLAHBAD");
    ASSERT_FALSE(view_state.healthy());
}

namespace {

mixxx::library::HeaderViewState::HeaderState* addColumn(
        mixxx::library::HeaderViewState* pState,
        const char* name,
        int size) {
    mixxx::library::HeaderViewState::HeaderState* pColumn = pState->add_header_state();
    pColumn->set_hidden(false);
    pColumn->set_size(size);
    pColumn->set_column_name(name);
    return pColumn;
}

QStringList columnNames(const HeaderViewState& state) {
    // Round-trip through the serialized form, that's all the state exposes.
    mixxx::library::HeaderViewState state_pb;
    const QByteArray array = QByteArray::fromBase64(state.saveState().toLatin1());
    EXPECT_TRUE(state_pb.ParseFromArray(array.constData(), array.size()));
    QStringList names;
    for (int i = 0; i < state_pb.header_state_size(); ++i) {
        names << QString::fromStdString(state_pb.header_state(i).column_name());
    }
    return names;
}

} // namespace

// A view that doesn't have all the columns must not drop the others from the
// layout that is shared by all views.
TEST_F(HeaderViewStateTest, MergeMissingColumnsKeepsForeignColumns) {
    // A playlist view, which has a track number column the library doesn't have.
    mixxx::library::HeaderViewState playlist_pb;
    addColumn(&playlist_pb, "position", 30);
    addColumn(&playlist_pb, "artist", 200);
    addColumn(&playlist_pb, "title", 300);

    // The library view, sorted the other way around.
    mixxx::library::HeaderViewState library_pb;
    addColumn(&library_pb, "title", 300);
    addColumn(&library_pb, "artist", 200);

    HeaderViewState merged(library_pb);
    merged.mergeMissingColumns(HeaderViewState(playlist_pb));

    // The order of the saving view wins, the track number keeps its place.
    EXPECT_EQ(QStringList({"position", "title", "artist"}), columnNames(merged));
}

// Columns which none of the saving view's columns are between have to keep
// their order.
TEST_F(HeaderViewStateTest, MergeMissingColumnsKeepsGroupOrder) {
    mixxx::library::HeaderViewState all_pb;
    addColumn(&all_pb, "artist", 200);
    addColumn(&all_pb, "album", 200);
    addColumn(&all_pb, "year", 60);
    addColumn(&all_pb, "title", 300);

    mixxx::library::HeaderViewState some_pb;
    addColumn(&some_pb, "artist", 200);
    addColumn(&some_pb, "title", 300);

    HeaderViewState merged(some_pb);
    merged.mergeMissingColumns(HeaderViewState(all_pb));

    EXPECT_EQ(QStringList({"artist", "album", "year", "title"}), columnNames(merged));
}

TEST_F(HeaderViewStateTest, MergeMissingColumnsPreservesSizes) {
    mixxx::library::HeaderViewState stored_pb;
    addColumn(&stored_pb, "artist", 200);
    addColumn(&stored_pb, "album", 111);
    stored_pb.mutable_header_state(1)->set_hidden(true);

    mixxx::library::HeaderViewState current_pb;
    addColumn(&current_pb, "artist", 222);

    HeaderViewState merged(current_pb);
    merged.mergeMissingColumns(HeaderViewState(stored_pb));

    mixxx::library::HeaderViewState expected_pb;
    addColumn(&expected_pb, "artist", 222);
    addColumn(&expected_pb, "album", 111);
    expected_pb.mutable_header_state(1)->set_hidden(true);

    EXPECT_EQ(HeaderViewState(expected_pb).saveState(), merged.saveState());
}

// Sorting is not part of the shared layout, it stays a per-view setting.
TEST_F(HeaderViewStateTest, SortIndicator) {
    mixxx::library::HeaderViewState shared_pb;
    addColumn(&shared_pb, "artist", 200);
    shared_pb.set_sort_indicator_shown(true);
    shared_pb.set_sort_indicator_section(3);
    shared_pb.set_sort_order(Qt::AscendingOrder);

    mixxx::library::HeaderViewState view_pb;
    addColumn(&view_pb, "artist", 200);
    view_pb.set_sort_indicator_shown(true);
    view_pb.set_sort_indicator_section(7);
    view_pb.set_sort_order(Qt::DescendingOrder);

    HeaderViewState shared(shared_pb);
    shared.adoptSortIndicator(HeaderViewState(view_pb));
    EXPECT_EQ(HeaderViewState(view_pb).saveState(), shared.saveState());

    // A view without a sort indicator must not inherit the previous one.
    mixxx::library::HeaderViewState unsorted_pb;
    addColumn(&unsorted_pb, "artist", 200);
    shared.adoptSortIndicator(HeaderViewState(unsorted_pb));

    mixxx::library::HeaderViewState expected_pb;
    addColumn(&expected_pb, "artist", 200);
    EXPECT_EQ(HeaderViewState(expected_pb).saveState(), shared.saveState());

    HeaderViewState cleared(shared_pb);
    cleared.clearSortIndicator();
    EXPECT_EQ(HeaderViewState(expected_pb).saveState(), cleared.saveState());
}
