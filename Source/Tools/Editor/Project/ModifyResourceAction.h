//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once

#include "../Core/CommonEditorActions.h"

namespace Urho3D
{

class ProjectEditor;

class ModifyResourceAction : public EditorAction
{
public:
    explicit ModifyResourceAction(ProjectEditor* project);
    void AddResource(Resource* resource);

    /// Implement EditorAction.
    /// @{
    bool IsComplete() const override { return false; }
    void Complete() override;
    void Redo() const override;
    void Undo() const override;
    bool MergeWith(const EditorAction& other) override;
    /// @}

private:
    struct ResourceData
    {
        StringHash resourceType_;
        ea::string fileName_;
        SharedByteVector bytes_;
    };

    void ApplyResourceData(const ea::string& resourceName, const ResourceData& data) const;

    WeakPtr<ProjectEditor> project_;
    Context* context_{};
    ea::unordered_map<ea::string, ResourceData> oldData_;
    ea::unordered_map<ea::string, ResourceData> newData_;

    mutable ea::function<void()> callback_;
};

}