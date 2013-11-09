/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 The MITRE Corporation                                      *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <openbr/openbr_plugin.h>

#include "bee.h"
#include "common.h"
#include "qtutils.h"

using namespace br;

/**** ALGORITHM_CORE ****/
struct AlgorithmCore
{
    QSharedPointer<Transform> transform;
    QSharedPointer<Distance> distance;

    AlgorithmCore(const QString &name)
    {
        this->name = name;
        init(name);
    }

    bool isClassifier() const
    {
        return distance.isNull();
    }

    void train(const File &input, const QString &model)
    {
        qDebug("Training on %s%s", qPrintable(input.flat()),
               model.isEmpty() ? "" : qPrintable(" to " + model));

        TemplateList data(TemplateList::fromGallery(input));

        // set the Train bool metadata, in case a Transform's project
        // needs to know if it's called during train or enroll
        for (int i=0; i<data.size(); i++)
            data[i].file.set("Train", true);

        if (transform.isNull()) qFatal("Null transform.");
        qDebug("%d training files", data.size());

        QTime time; time.start();
        qDebug("Training Enrollment");
        transform->train(data);

        if (!distance.isNull()) {
            qDebug("Projecting Enrollment");
            data >> *transform;

            qDebug("Training Comparison");
            distance->train(data);
        }

        if (!model.isEmpty()) {
            qDebug("Storing %s", qPrintable(QFileInfo(model).fileName()));
            store(model);
        }

        qDebug("Training Time (sec): %d", time.elapsed()/1000);
    }

    void store(const QString &model) const
    {
        // Create stream
        QByteArray data;
        QDataStream out(&data, QFile::WriteOnly);

        // Serialize algorithm to stream
        out << name;
        transform->store(out);
        const bool hasComparer = !distance.isNull();
        out << hasComparer;
        if (hasComparer) distance->store(out);

        // Compress and save to file
        QtUtils::writeFile(model, data, -1);
    }

    void load(const QString &model)
    {
        // Load from file and decompress
        QByteArray data;
        QtUtils::readFile(model, data, true);

        // Create stream
        QDataStream in(&data, QFile::ReadOnly);

        // Load algorithm
        in >> name; init(Globals->abbreviations.contains(name) ? Globals->abbreviations[name] : name);
        transform->load(in);
        bool hasDistance; in >> hasDistance;
        if (hasDistance) distance->load(in);
    }

    File getMemoryGallery(const File &file) const
    {
        return name + file.baseName() + file.hash() + ".mem";
    }

    FileList enroll(File input, File gallery = File())
    {
        qDebug("Enrolling %s%s", qPrintable(input.flat()),
               gallery.isNull() ? "" : qPrintable(" to " + gallery.flat()));

        FileList fileList;
        if (gallery.name.isEmpty()) {
            if (input.name.isEmpty()) return FileList();
            else                      gallery = getMemoryGallery(input);
        }

        QScopedPointer<Gallery> g(Gallery::make(gallery));
        if (g.isNull()) qFatal("Null gallery!");

        do {
            fileList.clear();

            if (gallery.contains("read") || gallery.contains("cache"))
                fileList = g->files();

            if (!fileList.isEmpty() && gallery.contains("cache"))
                return fileList;

            const TemplateList i(TemplateList::fromGallery(input));
            if (i.isEmpty()) return fileList; // Nothing to enroll

            if (transform.isNull()) qFatal("Null transform.");
            const int blocks = Globals->blocks(i.size());
            Globals->currentStep = 0;
            Globals->totalSteps = i.size();
            Globals->startTime.start();

            const bool noDuplicates = gallery.contains("noDuplicates");
            QStringList fileNames = noDuplicates ? fileList.names() : QStringList();
            const int subBlockSize = 4*std::max(1, Globals->parallelism);
            const int numSubBlocks = ceil(1.0*Globals->blockSize/subBlockSize);
            int totalCount = 0, failureCount = 0;
            double totalBytes = 0;
            for (int block=0; block<blocks; block++) {
                for (int subBlock = 0; subBlock<numSubBlocks; subBlock++) {
                    TemplateList data = i.mid(block*Globals->blockSize + subBlock*subBlockSize, subBlockSize);
                    if (data.isEmpty()) break;
                    if (noDuplicates)
                        for (int i=data.size()-1; i>=0; i--)
                            if (fileNames.contains(data[i].file.name))
                                data.removeAt(i);
                    const int numFiles = data.size();

                    data >> *transform;

                    g->writeBlock(data);
                    const FileList newFiles = data.files();
                    fileList.append(newFiles);

                    totalCount += newFiles.size();
                    failureCount += newFiles.failures();
                    totalBytes += data.bytes<double>();
                    Globals->currentStep += numFiles;
                    Globals->printStatus();
                }
            }

            const float speed = 1000 * Globals->totalSteps / Globals->startTime.elapsed() / std::max(1, abs(Globals->parallelism));
            if (!Globals->quiet && (Globals->totalSteps > 1))
                fprintf(stderr, "\rTIME ELAPSED (MINS) %f SPEED=%.1e  SIZE=%.4g  FAILURES=%d/%d  \n",
                        Globals->startTime.elapsed()/1000./60.,speed, totalBytes/totalCount, failureCount, totalCount);
            Globals->totalSteps = 0;
        } while (input.getBool("infinite"));

        return fileList;
    }

    void enroll(TemplateList &data)
    {
        if (transform.isNull()) qFatal("Null transform.");
        data >> *transform;
    }

    void retrieveOrEnroll(const File &file, QScopedPointer<Gallery> &gallery, FileList &galleryFiles)
    {
        if (!file.getBool("enroll") && (QStringList() << "gal" << "mem" << "template").contains(file.suffix())) {
            // Retrieve it
            gallery.reset(Gallery::make(file));
            galleryFiles = gallery->files();
        } else {
            // Was it already enrolled in memory?
            gallery.reset(Gallery::make(getMemoryGallery(file)));
            galleryFiles = gallery->files();
            if (!galleryFiles.isEmpty()) return;

            // Enroll it
            enroll(file);
            gallery.reset(Gallery::make(getMemoryGallery(file)));
            galleryFiles = gallery->files();
        }
    }

    void compare(File targetGallery, File queryGallery, File output)
    {
        qDebug("Comparing %s and %s%s", qPrintable(targetGallery.flat()),
               qPrintable(queryGallery.flat()),
               output.isNull() ? "" : qPrintable(" to " + output.flat()));

        if (output.exists() && output.get<bool>("cache", false)) return;
        if (queryGallery == ".") queryGallery = targetGallery;

        QScopedPointer<Gallery> t, q;
        FileList targetFiles, queryFiles;
        retrieveOrEnroll(targetGallery, t, targetFiles);
        retrieveOrEnroll(queryGallery, q, queryFiles);

        QList<int> partitionSizes;
        QList<File> outputFiles;
        if (output.contains("split")) {
            if (!output.fileName().contains("%1")) qFatal("Output file name missing split number place marker (%%1)");
            partitionSizes = output.getList<int>("split");
            for (int i=0; i<partitionSizes.size(); i++) {
                File splitOutputFile = output.name.arg(i);
                outputFiles.append(splitOutputFile);
            }
        }
        else outputFiles.append(output);

        QList<Output*> outputs;
        foreach (const File &outputFile, outputFiles) outputs.append(Output::make(outputFile, targetFiles, queryFiles));

        if (distance.isNull()) qFatal("Null distance.");
        Globals->currentStep = 0;
        Globals->totalSteps = double(targetFiles.size()) * double(queryFiles.size());
        Globals->startTime.start();

        int queryBlock = -1;
        bool queryDone = false;
        while (!queryDone) {
            queryBlock++;
            TemplateList queries = q->readBlock(&queryDone);

            QList<TemplateList> queryPartitions;
            if (!partitionSizes.empty()) queryPartitions = queries.partition(partitionSizes);
            else queryPartitions.append(queries);

            for (int i=0; i<queryPartitions.size(); i++) {
                int targetBlock = -1;
                bool targetDone = false;
                while (!targetDone) {
                    targetBlock++;

                    TemplateList targets = t->readBlock(&targetDone);

                    QList<TemplateList> targetPartitions;
                    if (!partitionSizes.empty()) targetPartitions = targets.partition(partitionSizes);
                    else targetPartitions.append(targets);

                    outputs[i]->setBlock(queryBlock, targetBlock);

                    distance->compare(targetPartitions[i], queryPartitions[i], outputs[i]);

                    Globals->currentStep += double(targets.size()) * double(queries.size());
                    Globals->printStatus();
                }
            }
        }

        qDeleteAll(outputs);

        const float speed = 1000 * Globals->totalSteps / Globals->startTime.elapsed() / std::max(1, abs(Globals->parallelism));
        if (!Globals->quiet && (Globals->totalSteps > 1)) fprintf(stderr, "\rSPEED=%.1e  \n", speed);
        Globals->totalSteps = 0;
    }

private:
    QString name;

    QString getFileName(const QString &description) const
    {
        const QString file = Globals->sdkPath + "/share/openbr/models/algorithms/" + description;
        return QFileInfo(file).exists() ? file : QString();
    }

    void init(const File &description)
    {
        // Check if a trained binary already exists for this algorithm
        const QString file = getFileName(description);
        if (!file.isEmpty()) return init(file);

        if (description.exists()) {
            qDebug("Loading %s", qPrintable(description.fileName()));
            load(description);
            return;
        }

        // Expand abbreviated algorithms to their full strings
        if (Globals->abbreviations.contains(description))
            return init(Globals->abbreviations[description]);

        //! [Parsing the algorithm description]
        QStringList words = QtUtils::parse(description.flat(), ':');
        if ((words.size() < 1) || (words.size() > 2)) qFatal("Invalid algorithm format.");
        //! [Parsing the algorithm description]

        if (description.getBool("distribute", true))
            words[0] = "DistributeTemplate(" + words[0] + ")";

        //! [Creating the template generation and comparison methods]
        transform = QSharedPointer<Transform>(Transform::make(words[0], NULL));
        if (words.size() > 1) distance = QSharedPointer<Distance>(Distance::make(words[1], NULL));
        //! [Creating the template generation and comparison methods]
    }
};


class AlgorithmManager : public Initializer
{
    Q_OBJECT

public:
    static QHash<QString, QSharedPointer<AlgorithmCore> > algorithms;
    static QMutex algorithmsLock;

    void initialize() const {}

    void finalize() const
    {
        algorithms.clear();
    }

    static QSharedPointer<AlgorithmCore> getAlgorithm(const QString &algorithm)
    {
        if (algorithm.isEmpty()) qFatal("No default algorithm set.");

        if (!algorithms.contains(algorithm)) {
            // Some algorithms are recursive, so we need to construct them outside the lock.
            QSharedPointer<AlgorithmCore> algorithmCore(new AlgorithmCore(algorithm));

            algorithmsLock.lock();
            if (!algorithms.contains(algorithm))
                algorithms.insert(algorithm, algorithmCore);
            algorithmsLock.unlock();
        }

        return algorithms[algorithm];
    }
};

QHash<QString, QSharedPointer<AlgorithmCore> > AlgorithmManager::algorithms;
QMutex AlgorithmManager::algorithmsLock;

BR_REGISTER(Initializer, AlgorithmManager)

bool br::IsClassifier(const QString &algorithm)
{
    qDebug("Checking if %s is a classifier", qPrintable(algorithm));
    return AlgorithmManager::getAlgorithm(algorithm)->isClassifier();
}

void br::Train(const File &input, const File &model)
{
    AlgorithmManager::getAlgorithm(model.get<QString>("algorithm"))->train(input, model);
}

FileList br::Enroll(const File &input, const File &gallery)
{
    return AlgorithmManager::getAlgorithm(gallery.get<QString>("algorithm"))->enroll(input, gallery);
}

void br::Enroll(TemplateList &tl)
{
    QString alg = tl.first().file.get<QString>("algorithm");
    AlgorithmManager::getAlgorithm(alg)->enroll(tl);
}

void br::Compare(const File &targetGallery, const File &queryGallery, const File &output)
{
    AlgorithmManager::getAlgorithm(output.get<QString>("algorithm"))->compare(targetGallery, queryGallery, output);
}

void br::Convert(const File &fileType, const File &inputFile, const File &outputFile)
{
    qDebug("Converting %s %s to %s", qPrintable(fileType.flat()), qPrintable(inputFile.flat()), qPrintable(outputFile.flat()));

    if (fileType == "Format") {
        QScopedPointer<Format> before(Factory<Format>::make(inputFile));
        QScopedPointer<Format> after(Factory<Format>::make(outputFile));
        after->write(before->read());
    } else if (fileType == "Gallery") {
        QScopedPointer<Gallery> before(Gallery::make(inputFile));
        QScopedPointer<Gallery> after(Gallery::make(outputFile));
        bool done = false;
        while (!done) after->writeBlock(before->readBlock(&done));
    } else if (fileType == "Output") {
        QString target, query;
        cv::Mat m = BEE::readSimmat(inputFile, &target, &query);
        const FileList targetFiles = TemplateList::fromGallery(target).files();
        const FileList queryFiles = TemplateList::fromGallery(query).files();

        if (targetFiles.size() != m.cols || queryFiles.size() != m.rows)
            qFatal("Similarity matrix and file size mismatch.");

        QSharedPointer<Output> o(Factory<Output>::make(outputFile));
        o->initialize(targetFiles, queryFiles);

        for (int i=0; i<queryFiles.size(); i++)
            for (int j=0; j<targetFiles.size(); j++)
                o->setRelative(m.at<float>(i,j), i, j);
    } else {
        qFatal("Unrecognized file type %s.", qPrintable(fileType.flat()));
    }
}

void br::Cat(const File &fileType, const QStringList &inputFiles, const File &outputFile)
{
    qDebug("Concatenating %d %s files to %s", inputFiles.size(), qPrintable(fileType.flat()), qPrintable(outputFile.flat()));

    if (fileType == "Gallery") {
        foreach (const QString &inputFile, inputFiles)
            if (inputFile == outputFile)
                qFatal("outputFile must not be in inputFiles.");

        QScopedPointer<Gallery> og(Gallery::make(outputFile));
        foreach (const QString &inputGallery, inputFiles) {
            QScopedPointer<Gallery> ig(Gallery::make(inputGallery));
            bool done = false;
            while (!done) og->writeBlock(ig->readBlock(&done));
        }
    } else if (fileType == "Output") {
        const QString catType = outputFile.get<QString>("catType");

        // Concatonate to the first simmat
        QStringList intputSimmats = inputFiles;
        QString target, query;
        cv::Mat catSimmat = BEE::readSimmat(intputSimmats[0], &target, &query);
        intputSimmats.removeFirst();

        FileList targetFiles(TemplateList::fromGallery(target).files());
        FileList queryFiles(TemplateList::fromGallery(query).files());

        foreach (const QString &inputSimmat, intputSimmats) {
            cv::Mat m = BEE::readSimmat(inputSimmat, &target, &query);
            const FileList targets = TemplateList::fromGallery(target).files();
            const FileList queries = TemplateList::fromGallery(query).files();

            if (targets.size() != m.cols || queries.size() != m.rows)
                qFatal("Similarity matrix (%i, %i) and file size (%i, %i) mismatch.", m.rows, m.cols, queries.size(), targets.size());

            // All matrices need to be doing the same thing
            if (catType == "colWise" /*We're trying to add more target comparisons for the same queries*/) {
                targetFiles.append(targets);
                cv::hconcat(catSimmat, m, catSimmat);
            } else if (catType == "rowWise" /*We're trying to add more query comparisons for the same targets*/) {
                queryFiles.append(queries);
                cv::vconcat(catSimmat, m, catSimmat);
            } else {
                qFatal("Unsupported concatonation type.");
            }
        }

        QSharedPointer<Output> o(Factory<Output>::make(outputFile));
        o->initialize(targetFiles, queryFiles);

        for (int i=0; i<queryFiles.size(); i++)
            for (int j=0; j<targetFiles.size(); j++)
                o->setRelative(catSimmat.at<float>(i,j), i, j);
    } else {
        qFatal("Unrecognized file type %s.", qPrintable(fileType.flat()));
    }
}

QSharedPointer<br::Transform> br::Transform::fromAlgorithm(const QString &algorithm)
{
    return AlgorithmManager::getAlgorithm(algorithm)->transform;
}

QSharedPointer<br::Distance> br::Distance::fromAlgorithm(const QString &algorithm)
{
    return AlgorithmManager::getAlgorithm(algorithm)->distance;
}

#include "core.moc"
